extends "res://tests/dynamic_lighting.gd"
# Explicit GI admission and live quality reallocation on a real GPU.
func _ready() -> void:
	get_window().size = Vector2i(641, 479)
	get_viewport().use_taa = false
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	var world := WorldEnvironment.new()
	world.environment = env
	add_child(world)
	camera = Camera3D.new()
	camera.position = Vector3(5, 5, 8)
	add_child(camera)
	camera.look_at(Vector3.ZERO)
	box(Vector3.ZERO, Vector3(4, 0.3, 4), material(Vector3.ONE * 0.7))
	var unsupported := material(Vector3.ONE)
	unsupported.set_shader_parameter("kiln_uniform_transport", false)
	box(Vector3(0, 1, 0), Vector3.ONE, unsupported)
	var dynamic_material := material(Vector3(0.7, 0.3, 0.1))
	var rigid := box(Vector3(2, 1, 0), Vector3.ONE, dynamic_material, true)
	gi = KilnGIWorld.new()
	gi.environment = env
	gi.set_ao_enabled(false)
	add_child(gi)
	var records := []
	for quality in [Vector2i(1, 256), Vector2i(8, 16), Vector2i(1, 256)]:
		gi.set_quality(quality.x, quality.y)
		await settle(20)
		var stats := gi.get_statistics()
		assert(stats.static_triangles == 12)
		assert(stats.rays_per_frame == quality.x and stats.convergence_samples == quality.y)
		records.append(stats)
	var before := gi.get_statistics()
	dynamic_material.set_shader_parameter("metalness", 1.0)
	await settle(4)
	var metal := gi.get_statistics()
	assert(metal.material_version > before.material_version)
	assert(metal.dynamic_version == before.dynamic_version)
	assert(metal.geometry_version == before.geometry_version)
	var replacement := SphereMesh.new()
	rigid.mesh = replacement
	await settle(4)
	var replaced := gi.get_statistics()
	assert(replaced.dynamic_triangles > metal.dynamic_triangles)
	assert(replaced.dynamic_version > metal.dynamic_version)
	assert(replaced.material_version == metal.material_version)
	replacement.radius = 0.8
	await settle(4)
	var rebuilt := gi.get_statistics()
	assert(rebuilt.dynamic_version > replaced.dynamic_version)
	assert(rebuilt.geometry_version == replaced.geometry_version)
	assert(gi.validate_bvh(8).passed)
	records.append({"before": before, "metal_change": metal, "mesh_replaced": replaced, "mesh_edited_and_rebuilt": rebuilt})
	var report := OS.get_environment("TEMP").path_join("kiln-admission.json") if OS.has_feature("windows") else "/tmp/kiln-admission.json"
	FileAccess.open(report, FileAccess.WRITE).store_string(JSON.stringify({"passed": true, "records": records}, "\t"))
	print("[KILN_ADMISSION] passed")
	get_tree().quit()
