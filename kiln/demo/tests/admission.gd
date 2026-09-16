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
	FileAccess.open("/tmp/kiln-admission.json", FileAccess.WRITE).store_string(JSON.stringify({"passed": true, "records": records}, "\t"))
	print("[KILN_ADMISSION] passed")
	get_tree().quit()
