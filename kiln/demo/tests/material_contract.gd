extends Node3D
# A separate chart keeps the original island composition intact.
func _ready() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.08, 0.09, 0.12)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var camera := Camera3D.new()
	camera.position = Vector3(0, 4, 13)
	add_child(camera)
	camera.look_at(Vector3(0, 0.8, 0))
	camera.make_current()
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, -30, 0)
	sun.shadow_enabled = true
	add_child(sun)
	var checker := Image.create(16, 16, false, Image.FORMAT_RGBA8)
	var normal := Image.create(16, 16, false, Image.FORMAT_RGB8)
	for y in 16:
		for x in 16:
			checker.set_pixel(x, y, Color(0.9, 0.25, 0.12) if (x / 8 + y / 8) % 2 else Color(0.15, 0.65, 0.8))
			normal.set_pixel(x, y, Color(0.5 + sin(x * TAU / 16.0) * 0.3, 0.5, 1.0))
	for i in 6:
		var mesh := MeshInstance3D.new()
		var sphere := SphereMesh.new()
		sphere.radius = 0.8
		sphere.height = 1.6
		mesh.mesh = sphere
		mesh.position = Vector3((i % 3 - 1) * 2.5, 1.0, -float(i / 3) * 2.5)
		var m := ShaderMaterial.new()
		if i < 3:
			m.shader = load("res://rendering/shaders/contract.gdshader")
			m.set_shader_parameter("checker", ImageTexture.create_from_image(checker))
			m.set_shader_parameter("normal_tex", ImageTexture.create_from_image(normal))
			m.set_shader_parameter("cutout", i == 1)
			m.set_shader_parameter("mapped_normal", i == 2)
		else:
			m.shader = load("res://rendering/shaders/%s.gdshader" % ["transparent", "additive", "refraction"][i - 3])
		mesh.material_override = m
		add_child(mesh)
	var floor_mesh := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(12, 12)
	floor_mesh.mesh = plane
	var floor_material := StandardMaterial3D.new()
	floor_material.albedo_color = Color(0.7, 0.7, 0.7)
	floor_mesh.material_override = floor_material
	add_child(floor_mesh)
	await get_tree().create_timer(5).timeout
	await RenderingServer.frame_post_draw
	var directory := "/tmp/kiln-material-contract-" + RenderingServer.get_current_rendering_method()
	DirAccess.make_dir_recursive_absolute(directory)
	get_viewport().get_texture().get_image().save_png(directory.path_join("chart.png"))
	get_tree().quit()
