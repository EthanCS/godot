extends "res://tests/dynamic_lighting.gd"
# Acceptance thresholds are evaluated by check_temporal.py, set before capture.
func picture(name: String) -> void:
	await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(output.path_join(name + ".png"))

func _ready() -> void:
	output = "/tmp/kiln-temporal"
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--output="): output = arg.trim_prefix("--output=")
	DirAccess.make_dir_recursive_absolute(output)
	get_window().size = Vector2i(800, 600)
	get_viewport().use_taa = true
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color.BLACK
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	camera = Camera3D.new()
	camera.position = Vector3(0, 5, 8)
	camera.fov = 50
	camera.far = 40
	add_child(camera)
	camera.look_at(Vector3(0, 0, 0))
	camera.make_current()
	var plaster := material(Vector3(0.7, 0.7, 0.7))
	box(Vector3(0, -0.15, 0), Vector3(12, 0.3, 10), plaster)
	emitter = box(Vector3(-1.4, 0.8, -0.5), Vector3(1, 1.3, 1), material(Vector3(0.1, 0.1, 0.1), Vector3(6, 0.1, 0.05)), true)
	var blocker := box(Vector3(0, 1, 1), Vector3(1.4, 2, 0.3), plaster, true)
	gi = KilnGIWorld.new()
	gi.environment = env
	gi.set_ao_enabled(false)
	gi.set_lighting(Vector3.UP, Vector3.ONE, 0, 0, 0)
	add_child(gi)
	await settle()
	await picture("lit")
	for i in 16:
		await settle(1)
		await picture("stationary_%02d" % i)
	emitter.material_override.set_shader_parameter("authored_emission", Vector3.ZERO)
	for i in 32:
		await settle(1)
		if i in [0, 3, 7, 15, 31]: await picture("off_%02d" % [i + 1])
	await settle()
	await picture("off_reference")
	emitter.material_override.set_shader_parameter("authored_emission", Vector3(6, 0.1, 0.05))
	await settle()
	for i in 60:
		blocker.position.x = i * 0.06
		camera.position.x = sin(i * 0.03) * 0.3
		await settle(1)
		if i % 5 == 0: await picture("motion_%02d" % i)
	await picture("motion_end")
	await settle(32)
	await picture("settled_32")
	await settle()
	await picture("settled_reference")
	gi.request_capture(output.path_join("buffers"))
	await settle(3)
	gi.reset_history()
	gi.request_capture(output.path_join("history_reset"))
	await settle(3)
	camera.position = Vector3(0, 4, 6)
	camera.look_at(Vector3(0, 0, 3))
	camera.fov = 20
	blocker.visible = false
	assert(not camera.is_position_in_frustum(emitter.global_position), "Emitter must be outside the camera frustum")
	await settle()
	await shot("offscreen_lit")
	emitter.visible = false
	await settle()
	await shot("offscreen_dark")
	print("[KILN_TEMPORAL] captures complete")
	get_tree().quit()
