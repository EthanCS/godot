extends Node3D
# GPU diagnostic run. All readbacks occur outside benchmark runs.
var gi: KilnGIWorld
var output := "/tmp/kiln-dynamic-check"
var emitter: MeshInstance3D
var light: OmniLight3D
var camera: Camera3D

func material(color: Vector3, emission := Vector3.ZERO) -> ShaderMaterial:
	var m := ShaderMaterial.new()
	m.shader = load("res://rendering/shaders/main_island_surface.gdshader")
	m.set_shader_parameter("tint_linear", color)
	m.set_shader_parameter("authored_emission", emission)
	return m

func box(pos: Vector3, size: Vector3, mat: ShaderMaterial, dynamic := false) -> MeshInstance3D:
	var mesh := MeshInstance3D.new()
	var shape := BoxMesh.new()
	shape.size = size
	mesh.mesh = shape
	mesh.position = pos
	mesh.material_override = mat
	mesh.set_meta("kiln_dynamic", dynamic)
	add_child(mesh)
	return mesh

func settle(count := 320) -> void:
	for i in count:
		await get_tree().process_frame
		await RenderingServer.frame_post_draw

func shot(label: String) -> void:
	var directory := output.path_join(label)
	gi.request_capture(directory)
	await settle(3)
	get_viewport().get_texture().get_image().save_png(directory.path_join("color.png"))
	FileAccess.open(directory.path_join("world.json"), FileAccess.WRITE).store_string(JSON.stringify(gi.get_statistics()))
	print("[KILN_CHECK] ", label)

func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--output="): output = arg.trim_prefix("--output=")
	get_window().size = Vector2i(640, 480)
	get_viewport().use_taa = false
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color.BLACK
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var world := WorldEnvironment.new()
	world.environment = env
	add_child(world)
	camera = Camera3D.new()
	camera.position = Vector3(7, 7, 11)
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 9
	camera.far = 40
	add_child(camera)
	camera.look_at(Vector3(0, 0.4, 0))
	camera.make_current()
	var plaster := material(Vector3(0.7, 0.7, 0.7))
	box(Vector3(0, -0.15, 0), Vector3(12, 0.3, 10), plaster)
	box(Vector3(0, 1.5, -3.5), Vector3(12, 3, 0.3), plaster)
	box(Vector3(-5.8, 1.5, 0), Vector3(0.3, 3, 7), plaster)
	emitter = box(Vector3(-2, 0.8, 0), Vector3(0.7, 1.3, 0.7), material(Vector3(0.1, 0.1, 0.1)), true)
	gi = KilnGIWorld.new()
	gi.environment = env
	gi.set_ao_enabled(false)
	gi.set_lighting(Vector3.UP, Vector3.ONE, 0, 0, 0)
	add_child(gi)
	await settle()
	await shot("00_dark")
	emitter.material_override.set_shader_parameter("authored_emission", Vector3(6, 0.1, 0.05))
	await settle()
	await shot("01_red_left")
	emitter.position.x = 2
	await settle()
	await shot("02_red_right")
	var screen := box(Vector3(2, 1, 0.65), Vector3(2.5, 2, 0.15), plaster, true)
	await settle()
	await shot("03_occluded")
	screen.visible = false
	emitter.material_override.set_shader_parameter("authored_emission", Vector3(0.05, 0.1, 6))
	await settle()
	await shot("04_blue")
	emitter.visible = false
	await settle(8)
	await shot("05_off_8")
	await settle(24)
	await shot("06_off_32")
	await settle()
	await shot("07_off_settled")
	light = OmniLight3D.new()
	light.position = Vector3(0, 2, -2.5)
	light.omni_range = 10
	light.light_energy = 4
	light.light_color = Color(1, 0.05, 0.02)
	add_child(light)
	await settle()
	await shot("08_omni")
	light.visible = false
	var spot := SpotLight3D.new()
	spot.position = Vector3(0, 2, 1)
	spot.spot_range = 10
	spot.spot_angle = 35
	spot.light_energy = 4
	spot.light_color = Color(0.02, 0.05, 1)
	add_child(spot)
	spot.look_at(Vector3(0, 1, -3.5))
	await settle()
	await shot("09_spot")
	spot.visible = false
	emitter.visible = true
	for frame in 180:
		emitter.position = Vector3(sin(frame * 0.04) * 2, 0.8, cos(frame * 0.04))
		emitter.rotation.y = frame * 0.05
		await settle(1)
	await shot("10_motion")
	await settle()
	await shot("11_settled")
	gi.reset_history()
	await settle()
	await shot("12_cold")
	gi.rebuild()
	await settle()
	await shot("13_rebuild")
	print("[KILN_CHECK] captures complete")
	get_tree().quit()
