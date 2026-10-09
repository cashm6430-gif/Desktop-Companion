extends Node3D

# This is an isolated rendering/rig gate, not the approved Whale Girl model.
# The review harness stages the canonical GLB into a fresh res://assets directory.
const MODEL_PATH := "res://assets/character.glb"
const DEMO_LENGTH := 16.0
const WINDOW_SIZE := Vector2i(360, 420)
const HostPeer = preload("res://host_peer.gd")
const HostBehavior = preload("res://host_behavior.gd")
const CAPTURE_PLAN := [
	{"time": 1.0, "name": "standing"},
	{"time": 1.50, "name": "blink"},
	{"time": 3.25, "name": "wave"},
	{"time": 5.65, "name": "half-sit"},
	{"time": 7.2, "name": "seated"},
	{"time": 9.7, "name": "rise"},
	{"time": 13.1, "name": "turn45"},
	{"time": 15.25, "name": "idle-return"},
]

var character: Node3D
var player: AnimationPlayer
var skeleton: Skeleton3D
var camera: Camera3D
var meshes: Array[MeshInstance3D] = []
var animation_map: Dictionary = {}
var shape_targets: Array[Dictionary] = []
var material_contract: Array[Dictionary] = []
var review_output := ""
var quit_after := -1.0
var time_elapsed := 0.0
var state := ""
var cycle := 0
var frame_index := 0
var capture_index := 0
var captures: Array[Dictionary] = []
var trace: Array[Dictionary] = []
var last_trace_time := -1.0
var last_mask_time := -1.0
var mask_polygon := PackedVector2Array()
var capture_busy := false
var finishing := false
var model_bounds: AABB
var errors: Array[String] = []
var is_gpu := false
var dragging := false
var drag_offset := Vector2i.ZERO
var window_interactions: Array[Dictionary] = []
var stool: MeshInstance3D
var frame_times_ms: Array[float] = []
var wall_started_usec := 0
var previous_frame_usec := 0
var window_region_ready := false
var alpha_mask_cost_ms: Array[float] = []
var face_smile := 0.10
var test_canvas: CanvasLayer
var record_frames := false
var sequence_frames: Array[Dictionary] = []
var focus_events: Array[Dictionary] = []
var startup_window_flags: Dictionary = {}
var focus_guard: Node = null
var fg_probes: Array = []
var review_kind := "demo"
var capture_plan: Array = CAPTURE_PLAN.duplicate(true)
var review_profile: Dictionary = {}
var host_port := 0
var host_token := ""
var host_peer: RefCounted
var host_behavior: RefCounted
var host_events: Array[Dictionary] = []
var host_capabilities: Array[String] = []
var host_capability_details: Dictionary = {}
var host_pose_owner := ""
var temporary_workstation: Node3D
var head_bone := -1
var window_size := WINDOW_SIZE
var hidden_review_meshes: Array[String] = []

func _ready() -> void:
	_parse_arguments()
	if host_port != 0 and (host_port < 1 or host_port > 65535 or host_token.length() != 64 or not host_token.is_valid_hex_number(false)):
		_fail("Host mode requires --host-port 1..65535 and a 64-character hexadecimal --host-token.")
		return
	wall_started_usec = Time.get_ticks_usec()
	get_viewport().transparent_bg = true
	is_gpu = DisplayServer.get_name() != "headless"
	if is_gpu:
		_setup_window()
	if not review_output.is_empty():
		var directory_error := DirAccess.make_dir_recursive_absolute(review_output)
		if directory_error != OK:
			_fail("Cannot create review output directory: %s" % directory_error)
			return
		if record_frames:
			DirAccess.make_dir_recursive_absolute(review_output.path_join("frames"))
	if record_frames and (not is_gpu or review_output.is_empty()):
		_fail("--record-frames requires a real GPU window and --review-output.")
		return
	_write_json("runtime-start.json", _runtime_info())
	if not ResourceLoader.exists(MODEL_PATH):
		_fail("No staged GLB at %s; use tools/review_toon_prototype.py." % MODEL_PATH)
		return
	var packed := load(MODEL_PATH) as PackedScene
	if packed == null:
		_fail("Staged GLB does not import as a PackedScene.")
		return
	character = packed.instantiate() as Node3D
	add_child(character)
	_discover_nodes(character)
	if skeleton == null or player == null:
		_fail("GLB requires both Skeleton3D and AnimationPlayer.")
		return
	for animation_name in player.get_animation_list():
		var suffix := String(animation_name).get_slice("/", String(animation_name).count("/"))
		if suffix.to_lower() in ["idle", "wave", "sit"]:
			animation_map[suffix.to_lower()] = animation_name
	for required in (["idle"] if review_kind == "head-style" or host_port != 0 else ["idle", "wave", "sit"]):
		if not animation_map.has(required):
			_fail("GLB is missing animation %s; available: %s" % [required, player.get_animation_list()])
			return
	player.callback_mode_process = AnimationMixer.ANIMATION_CALLBACK_MODE_PROCESS_MANUAL
	player.get_animation(animation_map["idle"]).loop_mode = Animation.LOOP_LINEAR
	for optional in ["wave", "sit"]:
		if animation_map.has(optional):
			player.get_animation(animation_map[optional]).loop_mode = Animation.LOOP_NONE
	head_bone = skeleton.find_bone("head")
	_install_toon_materials()
	_measure_model()
	_setup_world()
	_setup_stool()
	if host_port != 0:
		_setup_workstation()
	_setup_test_label()
	_enter_state("idle")
	player.advance(0.0)
	if host_port != 0:
		_setup_host()
	_write_json("runtime-start.json", _runtime_info())

func _setup_stool() -> void:
	# GLB sit-contact contract: ground Y=0, support top Y=.35, pelvis moves to Z=-.18.
	stool = MeshInstance3D.new()
	stool.name = "TemporarySitSupport"
	var box := BoxMesh.new()
	box.size = Vector3(0.56, 0.35, 0.36)
	stool.mesh = box
	stool.position = Vector3(0, 0.175, -0.18)
	var material := ShaderMaterial.new()
	material.shader = load("res://toon.gdshader")
	material.set_shader_parameter("base_color", Color(0.29, 0.40, 0.57))
	var outline := ShaderMaterial.new()
	outline.shader = load("res://outline.gdshader")
	material.next_pass = outline
	stool.material_override = material
	stool.visible = false
	add_child(stool)

func _parse_arguments() -> void:
	var arguments := OS.get_cmdline_user_args()
	var index := 0
	while index < arguments.size():
		match arguments[index]:
			"--review-output":
				if index + 1 < arguments.size():
					index += 1
					review_output = arguments[index]
			"--quit-after":
				if index + 1 < arguments.size():
					index += 1
					quit_after = float(arguments[index])
			"--record-frames":
				record_frames = true
			"--review-kind":
				if index + 1 < arguments.size():
					index += 1
					review_kind = arguments[index]
			"--review-profile":
				if index + 1 < arguments.size():
					index += 1
					var profile_file := FileAccess.open(arguments[index], FileAccess.READ)
					if profile_file != null:
						var profile: Variant = JSON.parse_string(profile_file.get_as_text())
						if profile is Dictionary:
							review_profile = profile
			"--host-port":
				if index + 1 < arguments.size():
					index += 1
					host_port = int(arguments[index])
			"--host-token":
				if index + 1 < arguments.size():
					index += 1
					host_token = arguments[index]
		index += 1
	if review_kind == "head-style":
		capture_plan = [{"time": 1.0, "name": "front"}, {"time": 2.0, "name": "left35"},
			{"time": 3.0, "name": "right35"}, {"time": 4.0, "name": "back"},
			{"time": 5.0, "name": "blink"}, {"time": 6.0, "name": "smile"}]
		var requested_size: Array = review_profile.get("window_size", [])
		if requested_size.size() == 2:
			window_size = Vector2i(clampi(int(requested_size[0]), 240, 1920), clampi(int(requested_size[1]), 240, 1920))
	if not review_output.is_empty() and quit_after < 0:
		quit_after = 7.0 if review_kind == "head-style" else DEMO_LENGTH

func _probe_fg(window: Window, where: String) -> void:
	# GetForegroundWindow-style probe (synchronous Win32 state, no message
	# pump needed). Recorded under fg_probes, never under focus_events, so
	# the gate criteria stay untouched.
	fg_probes.append({"at": where, "fg_is_self": window.has_focus(),
		"wall_time_usec": Time.get_ticks_usec()})

func _setup_window() -> void:
	var window := get_window()
	# Foreground guard (see focus-guard/DESIGN.md): a GDExtension node whose
	# restorer thread (started at SCENE init, before this window is shown)
	# keeps the desktop out of foreground vacuum and returns any kernel
	# forced assignment within ~2ms -- below Godot's per-frame focus check.
	if ClassDB.class_exists("FocusGuard"):
		focus_guard = ClassDB.instantiate("FocusGuard")
		add_child(focus_guard)
	startup_window_flags = {"project_no_focus": ProjectSettings.get_setting("display/window/size/no_focus"),
		"node_unfocusable_before_configuration": window.unfocusable,
		"native_no_focus_before_configuration": DisplayServer.window_get_flag(DisplayServer.WINDOW_FLAG_NO_FOCUS, window.get_window_id()),
		"has_focus_before_configuration": window.has_focus(),
		"boot_splash_show_image": ProjectSettings.get_setting("application/boot_splash/show_image"),
		"boot_splash_bg_color": str(ProjectSettings.get_setting("application/boot_splash/bg_color"))}
	window.focus_entered.connect(func() -> void: _record_focus_event("focus_entered"))
	window.focus_exited.connect(func() -> void: _record_focus_event("focus_exited"))
	_record_focus_event("before_window_configuration")
	# Preserve NO_FOCUS before any resizing or border/topmost style changes.
	DisplayServer.window_set_flag(DisplayServer.WINDOW_FLAG_NO_FOCUS, true, window.get_window_id())
	window.unfocusable = true
	_probe_fg(window, "no_focus+unfocusable")
	window.title = "3D technical prototype - drag / right click to close"
	window.size = window_size
	_probe_fg(window, "title+size")
	window.borderless = true
	_probe_fg(window, "borderless")
	window.always_on_top = true
	_probe_fg(window, "always_on_top")
	window.transparent = true
	_probe_fg(window, "transparent")
	var usable := DisplayServer.screen_get_usable_rect()
	window.position = usable.position + Vector2i(
		maxi(24, usable.size.x - window_size.x - 56),
		maxi(24, usable.size.y - window_size.y - 40))
	window.close_requested.connect(_finish)
	_probe_fg(window, "position")
	_record_focus_event("after_window_configuration")
	# Hand the guard the real native handle now that the window exists.
	# Without the extension the class is absent and the audit keeps catching
	# the steal.
	if focus_guard != null:
		focus_guard.configure(
			DisplayServer.window_get_native_handle(DisplayServer.WINDOW_HANDLE, window.get_window_id()),
			"" if review_output.is_empty() else review_output.path_join("focus-guard-events.jsonl"))
	_probe_fg(window, "after_guard_configure")

func _discover_nodes(node: Node) -> void:
	if node is Skeleton3D and skeleton == null:
		skeleton = node as Skeleton3D
	if node is AnimationPlayer and player == null:
		player = node as AnimationPlayer
	if node is MeshInstance3D:
		var mesh_node := node as MeshInstance3D
		meshes.append(mesh_node)
		if mesh_node.mesh != null:
			for index in mesh_node.mesh.get_blend_shape_count():
				var shape_name := String(mesh_node.mesh.get_blend_shape_name(index))
				shape_targets.append({"node": mesh_node, "index": index, "name": shape_name})
	for child in node.get_children():
		_discover_nodes(child)

func _install_toon_materials() -> void:
	var toon_shader := load("res://toon.gdshader") as Shader
	var outline_shader := load("res://outline.gdshader") as Shader
	for mesh_node in meshes:
		if mesh_node.mesh == null:
			continue
		if review_kind == "head-style" and bool(review_profile.get("hide_proxy_body", true)):
			var head_material := false
			for surface in mesh_node.mesh.get_surface_count():
				var original := mesh_node.get_active_material(surface)
				if original != null and original.resource_name.begins_with("Whale"):
					head_material = true
			mesh_node.visible = head_material
			if not head_material:
				hidden_review_meshes.append(String(mesh_node.name))
		for surface in mesh_node.mesh.get_surface_count():
			var source := mesh_node.get_active_material(surface) as StandardMaterial3D
			var color := Color(0.3, 0.5, 0.85)
			var source_name := "unknown"
			if source != null:
				color = source.albedo_color
				source_name = source.resource_name
			var toon := ShaderMaterial.new()
			toon.shader = toon_shader
			toon.set_shader_parameter("base_color", color)
			if source != null and source.albedo_texture != null:
				toon.set_shader_parameter("use_texture", true)
				toon.set_shader_parameter("base_texture", source.albedo_texture)
			var facial_detail := source_name.to_lower().contains("eye") or source_name.to_lower().contains("iris") or source_name.to_lower().contains("pupil") or source_name.to_lower().contains("highlight") or source_name.to_lower().contains("mouth")
			if facial_detail:
				toon.set_shader_parameter("shade_strength", 0.0)
			else:
				var outline := ShaderMaterial.new()
				outline.shader = outline_shader
				toon.next_pass = outline
			mesh_node.set_surface_override_material(surface, toon)
			material_contract.append({"mesh": String(mesh_node.name), "surface": surface,
				"source_material": source_name, "base_color": [color.r, color.g, color.b, color.a],
				"outline": not facial_detail})

func _measure_model() -> void:
	var initialized := false
	for mesh_node in meshes:
		if mesh_node.mesh == null or not mesh_node.visible:
			continue
		var local_bounds := mesh_node.get_aabb()
		for corner in 8:
			var point := mesh_node.global_transform * local_bounds.get_endpoint(corner)
			if not initialized:
				model_bounds = AABB(point, Vector3.ZERO)
				initialized = true
			else:
				model_bounds = model_bounds.expand(point)

func _setup_world() -> void:
	var world_environment := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_CLEAR_COLOR
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color.WHITE
	environment.ambient_light_energy = 0.7
	world_environment.environment = environment
	add_child(world_environment)
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-35, -30, 0)
	light.light_energy = 1.0
	light.shadow_enabled = false
	add_child(light)
	camera = Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.keep_aspect = Camera3D.KEEP_HEIGHT
	camera.size = maxf(2.8, model_bounds.size.y * 1.32)
	camera.near = 0.05
	camera.far = 20.0
	var center_y := model_bounds.position.y + model_bounds.size.y * 0.53
	camera.position = Vector3(0, center_y, 6)
	add_child(camera)
	camera.look_at(Vector3(0, center_y, 0), Vector3.UP)
	if review_kind == "head-style":
		var config: Dictionary = review_profile.get("camera", {})
		var default_y := model_bounds.position.y + model_bounds.size.y * 0.77
		var target: Array = config.get("target", [0, default_y, 0])
		var position_value: Array = config.get("position", [0, default_y, 6])
		if target.size() == 3 and position_value.size() == 3:
			camera.position = Vector3(float(position_value[0]), float(position_value[1]), float(position_value[2]))
			camera.look_at(Vector3(float(target[0]), float(target[1]), float(target[2])), Vector3.UP)
		camera.size = maxf(0.3, float(config.get("size", model_bounds.size.y * 0.66)))
	camera.current = true

func _setup_workstation() -> void:
	# Complete connected proxy props. No keyboard animation is claimed by this rig.
	temporary_workstation = Node3D.new()
	temporary_workstation.name = "TemporaryStaticWorkstation"
	add_child(temporary_workstation)
	_add_prop_box("DeskTop", Vector3(1.08, 0.045, 0.62), Vector3(0, 0.49, 0.30), Color(0.38, 0.47, 0.62))
	_add_prop_box("DeskFront", Vector3(1.08, 0.46, 0.035), Vector3(0, 0.25, 0.59), Color(0.29, 0.37, 0.54))
	_add_prop_box("LaptopBase", Vector3(0.45, 0.025, 0.29), Vector3(0, 0.525, 0.33), Color(0.10, 0.15, 0.29))
	_add_prop_box("LaptopHinge", Vector3(0.45, 0.028, 0.03), Vector3(0, 0.542, 0.465), Color(0.10, 0.15, 0.29))
	_add_prop_box("LaptopLid", Vector3(0.45, 0.28, 0.025), Vector3(0, 0.673, 0.465), Color(0.10, 0.15, 0.29))
	_add_prop_box("KeyboardInset", Vector3(0.39, 0.002, 0.16), Vector3(0, 0.539, 0.305), Color(0.19, 0.24, 0.38))
	temporary_workstation.visible = false

func _add_prop_box(label: String, size: Vector3, position_value: Vector3, color: Color) -> void:
	var mesh_node := MeshInstance3D.new()
	mesh_node.name = label
	var box := BoxMesh.new()
	box.size = size
	mesh_node.mesh = box
	mesh_node.position = position_value
	var material := ShaderMaterial.new()
	material.shader = load("res://toon.gdshader")
	material.set_shader_parameter("base_color", color)
	var outline := ShaderMaterial.new()
	outline.shader = load("res://outline.gdshader")
	outline.set_shader_parameter("outline_width", 0.003)
	material.next_pass = outline
	mesh_node.material_override = material
	temporary_workstation.add_child(mesh_node)

func _setup_host() -> void:
	host_behavior = HostBehavior.new()
	host_behavior.configure(animation_map.has("sit"), animation_map.has("wave"))
	host_behavior.acknowledgement.connect(_host_ack)
	host_capabilities = ["activity.idle", "activity.busy", "delete.react", "work.finished"]
	if animation_map.has("wave"):
		host_capabilities.append("wave")
	host_capability_details = {"model_sha256": FileAccess.get_sha256(MODEL_PATH),
		"character_status": "unapproved_proxy_or_candidate", "busy_style": "seated_static_workstation" if animation_map.has("sit") else "standing_expression_placeholder",
		"typing": false, "mouth_open_close": false, "finger_grasp": false,
		"delete_reaction": "small_head_nod_blink_smile_placeholder_no_file_operation",
		"facial_owner": "runtime_after_body_clip", "head_bone": head_bone >= 0,
		"blend_shapes": _available_shape_names()}
	host_peer = HostPeer.new()
	host_peer.configure(host_port, host_token, host_capabilities, host_capability_details)
	host_peer.message_received.connect(_host_message)
	host_peer.connection_changed.connect(_host_connection)
	host_peer.protocol_problem.connect(func(reason: String) -> void: _host_log({"event": "transport", "reason": reason}))
	state = "host-idle"

func _available_shape_names() -> Array:
	var names: Array = []
	for target in shape_targets:
		if not names.has(target["name"]):
			names.append(target["name"])
	return names

func _host_message(message: Dictionary) -> void:
	match String(message.get("type", "")):
		"activity.snapshot":
			host_behavior.snapshot(message)
			_host_log({"event": "snapshot", "revision": message.get("revision"), "busy": message.get("busy"), "active_turns": message.get("active_turns")})
		"action.request":
			host_behavior.request(message)
			_host_log({"event": "request", "request_id": message.get("request_id"), "name": message.get("name")})
		"action.cancel":
			host_behavior.cancel(message)
		"host.shutdown":
			_finish()

func _host_ack(request_id: String, status: String, reason: String) -> void:
	if host_peer != null:
		host_peer.send("action.ack", {"request_id": request_id, "status": status, "reason": reason})
	_host_log({"event": "ack", "request_id": request_id, "status": status, "reason": reason})

func _host_connection(connected: bool) -> void:
	_host_log({"event": "connected" if connected else "disconnected"})
	if not connected:
		host_behavior.connection_lost()

func _host_log(entry: Dictionary) -> void:
	entry["time"] = time_elapsed
	entry["wall_time_usec"] = Time.get_ticks_usec()
	host_events.append(entry)
	if host_events.size() > 1000:
		host_events.pop_front()

func _process_host(delta: float) -> void:
	host_peer.poll()
	if finishing:
		return
	host_behavior.update(delta)
	state = host_behavior.state
	var progress: float = host_behavior.sit_progress
	var owner := "sit" if progress > 0.0 else ("wave" if host_behavior.action_name == "wave" else "idle")
	if owner != host_pose_owner:
		host_pose_owner = owner
		player.play(animation_map[owner], 0.0)
		player.advance(0.0)
	if owner == "sit":
		player.seek(player.get_animation(animation_map["sit"]).length * progress, true)
		player.pause()
	elif owner == "wave":
		player.seek(player.get_animation(animation_map["wave"]).length * host_behavior.action_time / 2.5, true)
		player.pause()
	else:
		if not player.is_playing():
			player.play(animation_map["idle"], 0.0)
		player.advance(delta)
	stool.visible = progress > 0.001
	temporary_workstation.visible = progress >= 0.999 and (host_behavior.busy or host_behavior.action_name == "work.finished")
	character.rotation.y = 0.0
	if head_bone >= 0:
		var body_rotation := skeleton.get_bone_pose_rotation(head_bone)
		skeleton.set_bone_pose_rotation(head_bone, body_rotation * Quaternion(Vector3.RIGHT, host_behavior.head_pitch) * Quaternion(Vector3.UP, host_behavior.head_yaw))
	_set_face(time_elapsed, delta)

func _process_head_review(delta: float) -> void:
	state = "head-style"
	player.seek(0.0, true)
	player.pause()
	var yaw := 0.0
	if time_elapsed >= 1.5 and time_elapsed < 2.5:
		yaw = 35.0
	elif time_elapsed >= 2.5 and time_elapsed < 3.5:
		yaw = -35.0
	elif time_elapsed >= 3.5 and time_elapsed < 4.5:
		yaw = 180.0
	character.rotation.y = deg_to_rad(yaw + float(review_profile.get("model_yaw_degrees", 0.0)))
	_set_face(time_elapsed, delta)

func _setup_test_label() -> void:
	test_canvas = CanvasLayer.new()
	add_child(test_canvas)
	if review_kind == "head-style":
		test_canvas.visible = false
	var panel := Panel.new()
	panel.position = Vector2(29, 380)
	panel.size = Vector2(302, 32)
	panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.055, 0.08, 0.16, 0.85)
	style.set_corner_radius_all(6)
	panel.add_theme_stylebox_override("panel", style)
	test_canvas.add_child(panel)
	var label := Label.new()
	label.text = "临时技术简模 / 3D TEST\n拖动角色 · 右键退出"
	label.position = Vector2(0, 1)
	label.size = panel.size
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.add_theme_font_size_override("font_size", 11)
	var font := SystemFont.new()
	font.font_names = PackedStringArray(["Microsoft YaHei UI", "Microsoft YaHei", "Segoe UI"])
	label.add_theme_font_override("font", font)
	label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	panel.add_child(label)

func _process(delta: float) -> void:
	if player == null or finishing:
		return
	var tick := Time.get_ticks_usec()
	if is_gpu and previous_frame_usec > 0 and frame_index > 5:
		frame_times_ms.append(float(tick - previous_frame_usec) / 1000.0)
	previous_frame_usec = tick
	# Review time is an explicit fixed pose clock. GPU performance uses its separate wall clock.
	var animation_delta := 1.0 / 60.0 if not review_output.is_empty() and host_port == 0 else delta
	time_elapsed += animation_delta
	frame_index += 1
	if host_port != 0:
		_process_host(animation_delta)
	elif review_kind == "head-style":
		_process_head_review(animation_delta)
	else:
		var demo_time := fmod(time_elapsed, DEMO_LENGTH)
		var new_cycle := int(time_elapsed / DEMO_LENGTH)
		if new_cycle != cycle:
			cycle = new_cycle
			state = ""
		var next_state := _state_for_time(demo_time)
		if next_state != state:
			_enter_state(next_state)
		player.advance(animation_delta)
		_set_face(demo_time, animation_delta)
		if demo_time >= 11.5 and demo_time < 12.5:
			character.rotation.y = deg_to_rad(45.0) * smoothstep(11.5, 12.5, demo_time)
		elif demo_time >= 12.5 and demo_time < 14.0:
			character.rotation.y = deg_to_rad(45.0)
		elif demo_time >= 14.0 and demo_time < 14.75:
			character.rotation.y = deg_to_rad(45.0) * (1.0 - smoothstep(14.0, 14.75, demo_time))
		else:
			character.rotation.y = 0.0
	if time_elapsed - last_trace_time >= 1.0 / 20.0:
		last_trace_time = time_elapsed
		if not review_output.is_empty():
			trace.append(_pose_sample())
	if is_gpu and not capture_busy:
		var capture_due := not review_output.is_empty() and capture_index < capture_plan.size() and time_elapsed >= float(capture_plan[capture_index]["time"])
		var sequence_due := record_frames and frame_index % 4 == 0
		if capture_due or sequence_due or time_elapsed - last_mask_time >= 0.15:
			last_mask_time = time_elapsed
			_capture_frame(capture_due, sequence_due)
	if quit_after >= 0 and time_elapsed >= quit_after and not capture_busy:
		_finish()

func _state_for_time(t: float) -> String:
	if t < 2.0:
		return "idle"
	if t < 4.5:
		return "wave"
	if t < 6.9:
		return "sit"
	if t < 8.5:
		return "seated"
	if t < 11.0:
		return "rise"
	if t < 14.75:
		return "turn"
	return "idle"

func _enter_state(next_state: String) -> void:
	state = next_state
	if stool != null:
		stool.visible = state in ["sit", "seated", "rise"]
	match state:
		"idle", "turn":
			player.play(animation_map["idle"], 0.25)
		"wave":
			player.play(animation_map["wave"], 0.28, player.get_animation(animation_map["wave"]).length / 2.5)
		"sit":
			player.play(animation_map["sit"], 0.3, player.get_animation(animation_map["sit"]).length / 2.4)
		"seated":
			player.seek(player.get_animation(animation_map["sit"]).length, true)
			player.pause()
		"rise":
			player.play(animation_map["sit"], 0.2, -player.get_animation(animation_map["sit"]).length / 2.5, true)
			player.seek(player.get_animation(animation_map["sit"]).length, true)
	player.advance(0.0)

func _set_face(t: float, animation_delta: float) -> void:
	# Blend shape values are evaluated on the imported GLB mesh, not painted frames.
	var blink_phase := fmod(t, 3.2)
	var blink := 1.0 - smoothstep(0.03, 0.12, absf(blink_phase - 1.50))
	var smile := 0.10
	if state == "wave":
		smile = 0.80
	elif state in ["sit", "seated"]:
		smile = 0.40
	if host_port != 0 and host_behavior != null:
		smile = host_behavior.smile_target
		blink = maxf(blink, host_behavior.blink_override)
	elif review_kind == "head-style":
		blink = 1.0 if t >= 4.5 and t < 5.5 else 0.0
		smile = 1.0 if t >= 5.5 else 0.0
	face_smile = move_toward(face_smile, smile, animation_delta * 3.0)
	for target in shape_targets:
		var lower := String(target["name"]).to_lower()
		var mesh_node := target["node"] as MeshInstance3D
		if lower.contains("blink"):
			mesh_node.set_blend_shape_value(int(target["index"]), blink)
		elif lower.contains("smile"):
			mesh_node.set_blend_shape_value(int(target["index"]), face_smile)
		elif host_port != 0 or review_kind == "head-style":
			# All reviewed facial channels have one owner. Unknown channels stay neutral.
			mesh_node.set_blend_shape_value(int(target["index"]), 0.0)

func _input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		var button := event as InputEventMouseButton
		if button.button_index == MOUSE_BUTTON_RIGHT and button.pressed:
			window_interactions.append({"type": "right_click_close", "time": time_elapsed})
			if host_peer != null:
				host_peer.send("client.quit")
			_finish()
		if button.button_index == MOUSE_BUTTON_LEFT:
			dragging = button.pressed
			if dragging:
				drag_offset = DisplayServer.mouse_get_position() - get_window().position
				window_interactions.append({"type": "drag_start", "time": time_elapsed})
			else:
				window_interactions.append({"type": "drag_end", "time": time_elapsed, "position": _v2(get_window().position)})
	elif event is InputEventMouseMotion and dragging:
		var usable := DisplayServer.screen_get_usable_rect()
		var desired := DisplayServer.mouse_get_position() - drag_offset
		desired.x = clampi(desired.x, usable.position.x - window_size.x + 64, usable.end.x - 64)
		desired.y = clampi(desired.y, usable.position.y, usable.end.y - 64)
		get_window().position = desired

func _capture_frame(save_capture: bool, save_sequence: bool = false) -> void:
	capture_busy = true
	await RenderingServer.frame_post_draw
	if finishing:
		capture_busy = false
		return
	var image := get_viewport().get_texture().get_image()
	if image == null or image.is_empty():
		errors.append("GPU viewport readback returned no image.")
		capture_busy = false
		return
	image.convert(Image.FORMAT_RGBA8)
	var alpha_stats := _update_hit_region(image)
	if save_sequence:
		var sequence_filename := "frames/frame-%06d.png" % frame_index
		var sequence_path := review_output.path_join(sequence_filename)
		var sequence_result := image.save_png(sequence_path)
		if sequence_result == OK:
			sequence_frames.append({"file": sequence_filename, "frame": frame_index,
				"animation_time": time_elapsed, "source": "gpu_viewport_after_frame_post_draw",
				"sha256": FileAccess.get_sha256(sequence_path), "alpha": alpha_stats,
				"pose": _pose_sample()})
		else:
			errors.append("Sequence PNG write failed: %s (%s)" % [sequence_filename, sequence_result])
	if save_capture:
		var item: Dictionary = capture_plan[capture_index]
		var filename := String(item["name"]) + ".png"
		var result := image.save_png(review_output.path_join(filename))
		if result == OK:
			captures.append({"file": filename, "scheduled_time": item["time"], "actual_time": time_elapsed,
				"source": "gpu_viewport_after_frame_post_draw", "state": state,
				"alpha": alpha_stats, "pose": _pose_sample()})
		else:
			errors.append("PNG write failed: %s (%s)" % [filename, result])
		capture_index += 1
	capture_busy = false

func _update_hit_region(image: Image) -> Dictionary:
	var mask_started := Time.get_ticks_usec()
	var width := image.get_width()
	var height := image.get_height()
	var points := PackedVector2Array()
	var min_x := width
	var min_y := height
	var max_x := -1
	var max_y := -1
	var bitmap := BitMap.new()
	# Native C++ alpha contour extraction includes all alpha >= 1/255, including antialiasing.
	bitmap.create_from_image_alpha(image, 0.001)
	var visible_pixels := bitmap.get_true_bit_count()
	var transparent_pixels := width * height - visible_pixels
	for polygon in bitmap.opaque_to_polygons(Rect2i(0, 0, width, height), 0.0):
		for point in polygon:
			points.append(point)
			min_x = mini(min_x, int(point.x))
			max_x = maxi(max_x, int(point.x))
			min_y = mini(min_y, int(point.y))
			max_y = maxi(max_y, int(point.y))
	if points.size() >= 3:
		var hull := Geometry2D.convex_hull(points)
		var expanded := Geometry2D.offset_polygon(hull, 18.0, Geometry2D.JOIN_ROUND)
		if not expanded.is_empty():
			mask_polygon = expanded[0]
			for index in mask_polygon.size():
				mask_polygon[index].x = clampf(mask_polygon[index].x, 0, width)
				mask_polygon[index].y = clampf(mask_polygon[index].y, 0, height)
			DisplayServer.window_set_mouse_passthrough(mask_polygon, get_window().get_window_id())
			if not window_region_ready:
				window_region_ready = true
				_write_json("runtime-start.json", _runtime_info())
	alpha_mask_cost_ms.append(float(Time.get_ticks_usec() - mask_started) / 1000.0)
	return {"visible_pixels": visible_pixels, "transparent_pixels": transparent_pixels,
		"bbox": [min_x, min_y, max_x, max_y], "window_region": _polygon_json(),
		"character_and_outline_coverage": "complete_alpha_convex_hull_plus_18px_motion_margin",
		"background_inside_hull": "intercepts_mouse; outside_hull_passes_through",
		"clipped_at_viewport_edge": min_x <= 0 or min_y <= 0 or max_x >= width or max_y >= height}

func _pose_sample() -> Dictionary:
	var bones: Dictionary = {}
	if skeleton != null:
		for index in skeleton.get_bone_count():
			var pose := skeleton.get_bone_global_pose(index)
			bones[String(skeleton.get_bone_name(index))] = {
				"position": _v3(skeleton.get_bone_pose_position(index)),
				"rotation_xyzw": _q(skeleton.get_bone_pose_rotation(index)),
				"global_position": _v3(pose.origin),
				"scale": _v3(skeleton.get_bone_pose_scale(index)),
			}
	var shapes: Dictionary = {}
	for target in shape_targets:
		var mesh_node := target["node"] as MeshInstance3D
		shapes[String(mesh_node.name) + "/" + String(target["name"])] = mesh_node.get_blend_shape_value(int(target["index"]))
	return {"time": time_elapsed, "frame": frame_index, "state": state,
		"host": {"enabled": host_port != 0, "connected": host_peer.connected if host_peer != null else false,
			"busy": host_behavior.busy if host_behavior != null else false,
			"sit_progress": host_behavior.sit_progress if host_behavior != null else 0,
			"action": host_behavior.action_name if host_behavior != null else "",
			"workstation_visible": temporary_workstation.visible if temporary_workstation != null else false},
		"animation": String(player.assigned_animation) if player != null else "",
		"animation_position": player.current_animation_position if player != null else 0,
		"animation_speed": player.get_playing_speed() if player != null else 0,
		"character_yaw_degrees": rad_to_deg(character.rotation.y) if character != null else 0,
		"bones": bones, "blend_shapes": shapes}

func _runtime_info() -> Dictionary:
	var window := get_window()
	var info: Dictionary = {"pid": OS.get_process_id(), "godot_version": Engine.get_version_info(),
		"os": OS.get_name(), "display_server": DisplayServer.get_name(),
		"window_id": window.get_window_id(), "native_handle": 0,
		"window_region_ready": window_region_ready,
		"startup_window_flags": startup_window_flags,
		"focus_events": focus_events,
		"headless": not is_gpu, "model_path": MODEL_PATH,
		"window": {"size": _v2(window.size), "position": _v2(window.position),
			"borderless": window.borderless, "always_on_top": window.always_on_top,
			"transparent": window.transparent, "unfocusable": window.unfocusable,
			"has_focus": window.has_focus(),
			"viewport_transparent_bg": get_viewport().transparent_bg},
		"renderer": RenderingServer.get_current_rendering_method(),
		"rendering_driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": RenderingServer.get_video_adapter_name(),
		"adapter_vendor": RenderingServer.get_video_adapter_vendor(),
		"adapter_api_version": RenderingServer.get_video_adapter_api_version(),
		"rendering_device": "unavailable_in_OpenGL_Compatibility"}
	if is_gpu:
		info["native_handle"] = DisplayServer.window_get_native_handle(DisplayServer.WINDOW_HANDLE, window.get_window_id())
		info["window"]["transparency_available"] = DisplayServer.is_window_transparency_available()
	if character != null:
		info["model_sha256"] = FileAccess.get_sha256(MODEL_PATH)
		info["animation_names"] = Array(player.get_animation_list()) if player != null else []
		info["bone_count"] = skeleton.get_bone_count() if skeleton != null else 0
		info["materials"] = material_contract
		info["rest_bounds"] = {"position": _v3(model_bounds.position), "size": _v3(model_bounds.size)}
	info["review_kind"] = review_kind
	info["review_profile"] = review_profile
	info["hidden_review_meshes"] = hidden_review_meshes
	info["review_visibility_scope"] = "head_geometry_only_other_materials_hidden_in_scene" if review_kind == "head-style" and bool(review_profile.get("hide_proxy_body", true)) else "full_model"
	info["host_mode"] = {"enabled": host_port != 0, "address": "127.0.0.1" if host_port != 0 else "",
		"port": host_port, "capabilities": host_capabilities, "capability_details": host_capability_details}
	return info

func _finish() -> void:
	if finishing:
		return
	finishing = true
	if host_peer != null:
		host_peer.close()
	# Main Window.hide() is forbidden by Godot. Hide every visual atomically instead.
	# Do not free the model's individual parts while its last frame remains visible.
	visible = false
	if test_canvas != null:
		test_canvas.visible = false
	var transparent_frame_presented := false
	var shutdown_alpha_pixels := -1
	if is_gpu:
		await RenderingServer.frame_post_draw
		transparent_frame_presented = true
		var shutdown_image := get_viewport().get_texture().get_image()
		if shutdown_image != null and not shutdown_image.is_empty():
			var shutdown_bitmap := BitMap.new()
			shutdown_bitmap.create_from_image_alpha(shutdown_image, 0.001)
			shutdown_alpha_pixels = shutdown_bitmap.get_true_bit_count()
			if not review_output.is_empty():
				shutdown_image.save_png(review_output.path_join("shutdown-transparent.png"))
	var report := _runtime_info()
	report["status"] = "technical_prototype_pending_visual_review" if errors.is_empty() else "failed"
	report["visual_capture_completed"] = is_gpu and captures.size() == capture_plan.size() and errors.is_empty()
	report["manual_desktop_composite_review"] = "pending"
	report["approved_character"] = false
	report["duration_seconds"] = time_elapsed
	report["wall_duration_seconds"] = float(Time.get_ticks_usec() - wall_started_usec) / 1000000.0
	report["clock"] = {"mode": "fixed_animation_clock" if not review_output.is_empty() and host_port == 0 else "wall_delta_animation_clock",
		"animation_dt_seconds": 1.0 / 60.0 if not review_output.is_empty() and host_port == 0 else null,
		"animation_frames": frame_index}
	report["performance"] = _timing_statistics(frame_times_ms) if is_gpu else {"available": false, "reason": "headless_has_no_render_performance"}
	report["performance_scope"] = "runtime_including_review_readback_and_png_write; not_production_fps"
	report["alpha_mask_performance"] = _timing_statistics(alpha_mask_cost_ms) if is_gpu else {"available": false}
	report["capture_plan"] = capture_plan
	report["captures"] = captures
	report["trace_file"] = "trace.json"
	report["trace_samples"] = trace.size()
	report["sequence"] = {"enabled": record_frames, "frame_count": sequence_frames.size(),
		"animation_fps": 15, "stride_in_fixed_dt_frames": 4,
		"manifest_file": "sequence-manifest.json", "frames_directory": "frames",
		"source": "gpu_viewport_after_frame_post_draw"}
	report["animation_evaluation"] = "imported_AnimationPlayer_manual_advance_with_engine_crossfade"
	report["window_interactions"] = window_interactions
	report["sit_support"] = {"temporary_geometry": "box", "top_y": 0.35,
		"center": [0, 0.175, -0.18], "size": [0.56, 0.35, 0.36],
		"visible_states": ["sit", "seated", "rise"]}
	report["shutdown_strategy"] = "hide_all_visual_content_then_frame_post_draw_then_quit"
	report["shutdown_gpu_frame_presented"] = transparent_frame_presented
	report["shutdown_visible_alpha_pixels"] = shutdown_alpha_pixels
	report["native_window_hide"] = "not_used_main_window_visibility_is_fixed_by_Godot"
	report["errors"] = errors
	report["host_events"] = host_events
	report["host_review_limit"] = "snapshot-driven proxy, static work, no gripping/biting/typing capability claimed"
	if focus_guard != null:
		report["focus_guard"] = focus_guard.get_stats()
	else:
		report["focus_guard"] = {"available": false}
	report["fg_probes"] = fg_probes
	_write_json("trace.json", {"schema": 1, "samples": trace})
	if record_frames:
		_write_json("sequence-manifest.json", {"schema": 1, "animation_fps": 15,
			"animation_dt_seconds": 1.0 / 60.0, "frame_stride": 4, "frames": sequence_frames})
	_write_json("report.json", report)
	get_tree().quit(0 if errors.is_empty() else 1)

func _fail(message: String) -> void:
	errors.append(message)
	push_error(message)
	_finish()

func _write_json(filename: String, value: Variant) -> void:
	if review_output.is_empty():
		return
	var handle := FileAccess.open(review_output.path_join(filename), FileAccess.WRITE)
	if handle == null:
		push_error("Cannot write %s" % filename)
		return
	handle.store_string(JSON.stringify(value, "\t"))

func _v2(value: Vector2i) -> Array:
	return [value.x, value.y]

func _v3(value: Vector3) -> Array:
	return [value.x, value.y, value.z]

func _q(value: Quaternion) -> Array:
	return [value.x, value.y, value.z, value.w]

func _polygon_json() -> Array:
	var result: Array = []
	for point in mask_polygon:
		result.append([point.x, point.y])
	return result

func _timing_statistics(values: Array[float]) -> Dictionary:
	if values.is_empty():
		return {"available": false, "samples": 0}
	var sorted := values.duplicate()
	sorted.sort()
	var total := 0.0
	for value in sorted:
		total += value
	return {"available": true, "samples": sorted.size(), "unit": "milliseconds",
		"p50": sorted[int((sorted.size() - 1) * 0.50)],
		"p95": sorted[int((sorted.size() - 1) * 0.95)],
		"max": sorted[-1], "mean": total / float(sorted.size())}

func _record_focus_event(event_name: String) -> void:
	focus_events.append({"event": event_name, "animation_time": time_elapsed,
		"wall_time_usec": Time.get_ticks_usec(), "has_focus": get_window().has_focus(),
		"native_no_focus": DisplayServer.window_get_flag(DisplayServer.WINDOW_FLAG_NO_FOCUS, get_window().get_window_id())})
