extends "res://tests/dynamic_lighting.gd"
func _ready() -> void:
	output = "/tmp/kiln-shadow-contact"
	DirAccess.make_dir_recursive_absolute(output)
	get_window().size = Vector2i(960, 720)
	get_viewport().use_taa = false
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
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
	var sun := DirectionalLight3D.new()
	sun.light_energy = 1.1
	sun.light_color = Color("ffe3b8")
	sun.shadow_normal_bias = 0.65
	sun.light_angular_distance = 0.6
	sun.directional_shadow_mode = DirectionalLight3D.SHADOW_ORTHOGONAL
	add_child(sun)
	var cases := [["back", Vector3(-35, 120, 0), Vector3(0, 0, -3.3), Vector3.RIGHT], ["side", Vector3(-50, -30, 0), Vector3(-5.6, 0, 0), Vector3.BACK]]
	var results := {}
	var passed := true
	for item in cases:
		sun.rotation_degrees = item[1]
		sun.shadow_enabled = true
		await settle(12)
		var shadowed := get_viewport().get_texture().get_image()
		shadowed.save_png(output.path_join(item[0] + ".png"))
		sun.shadow_enabled = false
		await settle(3)
		var unshadowed := get_viewport().get_texture().get_image()
		var shadow := 0.0
		var lit := 0.0
		var count := 0
		for i in range(-40, 41):
			var uv := Vector2i(camera.unproject_position(item[2] + item[3] * (float(i) * 0.025)))
			if uv.x < 0 or uv.y < 0 or uv.x >= shadowed.get_width() or uv.y >= shadowed.get_height(): continue
			shadow += shadowed.get_pixelv(uv).srgb_to_linear().r
			lit += unshadowed.get_pixelv(uv).srgb_to_linear().r
			count += 1
		results[item[0]] = {"ratio": shadow / maxf(lit, 0.0001), "lit_mean": lit / maxi(count, 1), "samples": count}
		passed = passed and count >= 40 and lit / maxi(count, 1) > 0.1 and shadow / maxf(lit, 0.0001) < 0.02
	results.passed = passed
	FileAccess.open(output.path_join("checks.json"), FileAccess.WRITE).store_string(JSON.stringify(results, "\t"))
	print("[KILN_SHADOW] ", results)
	get_tree().quit(0 if passed else 1)
