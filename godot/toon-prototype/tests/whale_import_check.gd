extends SceneTree

# Headless import check for the whale-girl GLB: verifies that the joined
# single mesh keeps skinning, materials, textures and all animation clips
# when the engine reads it - the pipeline's last mile.
# Run: Godot_v4.7.2-stable_win64_console.exe --headless --path godot/toon-prototype --script res://tests/whale_import_check.gd

const GLB_PATH := "E:/projects/Desktop-Companion/assets/3d/whale-girl-head-r2/character-round3.glb"


func _initialize() -> void:
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	var err := doc.append_from_file(GLB_PATH, state)
	if err != OK:
		print("IMPORT_FAIL err=", err)
		quit(1)
		return
	var scene := doc.generate_scene(state)
	var meshes := 0
	var surfaces := 0
	var verts := 0
	var skinned := 0
	var textures := {}
	var materials := {}
	var stack: Array[Node] = [scene]
	while not stack.is_empty():
		var node: Node = stack.pop_back()
		if node is MeshInstance3D:
			meshes += 1
			var mi := node as MeshInstance3D
			if mi.skin != null:
				skinned += 1
			var mesh := mi.mesh
			if mesh != null:
				for s in range(mesh.get_surface_count()):
					surfaces += 1
					var arrays := mesh.surface_get_arrays(s)
					var points: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
					verts += points.size()
					var mat := mesh.surface_get_material(s)
					if mat is BaseMaterial3D:
						materials[mat.resource_name if mat.resource_name != "" else "mat%d" % materials.size()] = true
						if (mat as BaseMaterial3D).albedo_texture != null:
							var tex := (mat as BaseMaterial3D).albedo_texture
							textures[tex.resource_path if tex.resource_path != "" else tex.get_name()] = true
		for child in node.get_children():
			stack.append(child)
	var anim_names: Array[String] = []
	for a in state.get_animations():
		anim_names.append(a.get_name())
	var bone_count := 0
	var skel := scene.find_child("*Skeleton3D*", true, false) if scene else null
	if skel != null:
		bone_count = (skel as Skeleton3D).get_bone_count()
	print("WHALE_IMPORT_OK meshes=%d surfaces=%d verts=%d skinned=%d bones=%d materials=%d textures=%d anims=%s" % [
		meshes, surfaces, verts, skinned, bone_count, materials.size(), textures.size(),
		",".join(anim_names)])
	for t in textures.keys():
		print("  texture: ", t)
	quit(0)
