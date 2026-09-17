extends Node3D
## Focused resource invalidation tests; Sponza remains the visual acceptance scene.
var gi: KilnGIWorld
var records: Array = []
var output := ""

func settle() -> void:
	for i in 5:
		await get_tree().process_frame
		await RenderingServer.frame_post_draw

func stats() -> Dictionary:
	var s := gi.get_statistics()
	records.append(s)
	return s

func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--output="): output = arg.trim_prefix("--output=")
	assert(output != "")
	DirAccess.make_dir_recursive_absolute(output)
	var environment := Environment.new()
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)
	var camera := Camera3D.new()
	camera.position = Vector3(0, 2, 6)
	add_child(camera)
	camera.look_at(Vector3.ZERO)
	var mesh := SphereMesh.new()
	mesh.radial_segments = 64
	mesh.rings = 32
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.9, 0.1, 0.1))
	var texture := ImageTexture.create_from_image(image)
	var material := StandardMaterial3D.new()
	material.albedo_texture = texture
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.material_override = material
	add_child(instance)
	gi = KilnGIWorld.new()
	gi.environment = environment
	add_child(gi)
	await settle()
	var initial := stats()
	assert(initial.ray_geometry == "original_scene_meshes" and initial.static_triangles > 0)
	assert(not mesh.has_meta("kiln_gi_proxy"))
	material.albedo_color = Color(0.1, 0.8, 0.1)
	await settle()
	var tint := stats()
	assert(tint.material_version > initial.material_version and tint.geometry_version == initial.geometry_version)
	assert(tint.hardware_blas_builds == initial.hardware_blas_builds, "Color edits must not rebuild hardware geometry")
	image.fill(Color(0.1, 0.1, 0.9))
	texture.update(image)
	await settle()
	var updated := stats()
	assert(updated.material_version > tint.material_version and updated.geometry_version == tint.geometry_version)
	var other := StandardMaterial3D.new()
	other.emission_enabled = true
	other.emission = Color.GREEN
	instance.material_override = other
	await settle()
	var replaced := stats()
	assert(replaced.material_version > updated.material_version and replaced.geometry_version == updated.geometry_version)
	mesh.radius = 0.7 # No explicit rebuild: the mesh's changed signal invalidates the original-mesh upload.
	await settle()
	var geometry := stats()
	assert(geometry.geometry_version > replaced.geometry_version and geometry.mesh_uploads > replaced.mesh_uploads)
	instance.position.x = 1.0 # Static transforms are detected too.
	await settle()
	var moved := stats()
	assert(moved.geometry_version > geometry.geometry_version)
	assert(moved.mesh_uploads == geometry.mesh_uploads)
	assert(gi.validate_bvh(128).passed)
	instance.visible = false
	await settle()
	var hidden := stats()
	assert(hidden.static_triangles == 0 and hidden.source_triangles_unique == 0)
	instance.visible = true
	await settle()
	assert(stats().static_triangles > 0)
	for backend in [1, 2, 0, 1, 2]:
		gi.set_query_backend(backend)
		await settle()
		var selected := stats()
		if backend == 1:
			assert(selected.backend == "compute_software_bvh")
		elif selected.hardware_ray_query_available:
			assert(selected.backend == "hardware_ray_query")
	gi.request_capture(output.path_join("buffers"))
	await settle()
	FileAccess.open(output.path_join("checks.json"), FileAccess.WRITE).store_string(JSON.stringify({"passed": true, "records": records}, "\t"))
	print("[SCENE_GEOMETRY_LIFECYCLE] passed")
	get_tree().quit()
