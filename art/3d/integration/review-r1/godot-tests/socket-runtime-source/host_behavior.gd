extends RefCounted

# One base pose clock owns the lower body. Event overlays never scale/translate bones.
signal acknowledgement(request_id: String, status: String, reason: String)

var busy := false
var sit_progress := 0.0
var state := "host-idle"
var action_name := ""
var action_id := ""
var action_time := 0.0
var action_duration := 0.0
var head_pitch := 0.0
var head_yaw := 0.0
var blink_override := 0.0
var smile_target := 0.10
var can_sit := false
var can_wave := false
var _last_revision := -1
var _seen_requests: Dictionary = {}
var _deadline_seconds := 12.0

func configure(sit_available: bool, wave_available: bool) -> void:
	can_sit = sit_available
	can_wave = wave_available

func snapshot(message: Dictionary) -> void:
	var revision: int = int(message.get("revision", -1))
	if revision <= _last_revision:
		return
	if not message.get("busy") is bool:
		return
	_last_revision = revision
	busy = bool(message["busy"])
	if busy and action_name in ["wave", "work.finished"]:
		_interrupt("busy_resumed")

func request(message: Dictionary) -> void:
	var request_id := String(message.get("request_id", ""))
	var name := String(message.get("name", ""))
	if request_id.is_empty() or _seen_requests.has(request_id):
		return
	_seen_requests[request_id] = true
	if name not in ["delete.react", "work.finished", "wave"]:
		acknowledgement.emit(request_id, "rejected", "unsupported_action")
		return
	if action_name == "delete.react" and name != "delete.react":
		acknowledgement.emit(request_id, "rejected", "higher_priority_action_active")
		return
	if name == "wave" and (not can_wave or busy or sit_progress > 0.001):
		acknowledgement.emit(request_id, "rejected", "wave_requires_idle_standing_rig")
		return
	if name == "work.finished" and busy:
		acknowledgement.emit(request_id, "rejected", "activity_still_busy")
		return
	if not action_id.is_empty():
		_interrupt("replaced_by_" + name)
	action_id = request_id
	action_name = name
	action_time = 0.0
	action_duration = 2.5 if name == "wave" else (1.0 if name == "work.finished" else 1.2)
	_deadline_seconds = clampf(float(message.get("ttl_ms", 12000)) / 1000.0, 0.05, 12.0)
	acknowledgement.emit(action_id, "started", "")

func cancel(message: Dictionary) -> void:
	if String(message.get("request_id", "")) == action_id and not action_id.is_empty():
		_interrupt(String(message.get("reason", "cancelled")))

func connection_lost() -> void:
	busy = false
	_last_revision = -1
	_seen_requests.clear()
	_interrupt("connection_lost")

func update(delta: float) -> void:
	if not action_id.is_empty():
		action_time += delta
		if _deadline_seconds < action_duration and action_time >= _deadline_seconds:
			_interrupt("deadline")
		elif action_time >= action_duration:
			var completed := action_id
			var reason := "placeholder_expression_no_mouth_or_grasp" if action_name == "delete.react" else ""
			action_id = ""
			action_name = ""
			acknowledgement.emit(completed, "finished", reason)
	var sit_target := 1.0 if busy and can_sit else 0.0
	# Return gaze precedes rising; an incoming busy snapshot interrupts it cleanly.
	if action_name != "work.finished" or sit_progress < 0.999:
		sit_progress = move_toward(sit_progress, sit_target, delta / 2.4)
	state = "host-work" if busy else "host-idle"
	if can_sit and sit_progress > 0.001 and sit_progress < 0.999:
		state = "host-enter-work" if busy else "host-exit-work"
	elif can_sit and sit_progress >= 0.999:
		state = "host-work" if busy else "host-return-gaze"
	if busy and not can_sit:
		state = "host-busy-standing"
	var pitch_target := 0.0
	var yaw_target := 0.0
	blink_override = 0.0
	smile_target = 0.35 if busy else 0.10
	if action_name == "delete.react":
		var pulse := sin(PI * clampf(action_time / action_duration, 0.0, 1.0))
		pitch_target = -deg_to_rad(7.0) * pulse
		blink_override = sin(PI * clampf((action_time - 0.3) / 0.45, 0.0, 1.0))
		smile_target = 0.65 * pulse
	elif action_name == "work.finished":
		yaw_target = deg_to_rad(-12.0) * sin(PI * clampf(action_time / action_duration, 0.0, 1.0))
		smile_target = 0.75
	elif action_name == "wave":
		smile_target = 0.8
	head_pitch = move_toward(head_pitch, pitch_target, delta * 1.2)
	head_yaw = move_toward(head_yaw, yaw_target, delta * 1.8)

func _interrupt(reason: String) -> void:
	if action_id.is_empty():
		return
	var interrupted := action_id
	action_id = ""
	action_name = ""
	acknowledgement.emit(interrupted, "interrupted", reason)
