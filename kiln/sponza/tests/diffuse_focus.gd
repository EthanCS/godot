extends SceneTree
## Sun/sky diffuse only. No NRD, reflections, local lights or emissive workload.
## Use --raw-cache to bypass screen/temporal reconstruction (cache sharing stays enabled) independently.

func _initialize() -> void:
	ProjectSettings.set_setting("rendering/kiln/nrd", false)
	ProjectSettings.set_setting("rendering/kiln/surfel_specular", false)
	call_deferred("run")

func run() -> void:
	var scene = load("res://sponza.tscn").instantiate()
	root.add_child(scene)
	current_scene = scene
	assert(scene.output != "")
	root.set_flag(Window.FLAG_ALWAYS_ON_TOP, true)
	scene.controls.hide()
	scene.animate = false
	scene.orbit = false
	scene.time = 0.0
	scene._set_tod(0.25)
	var camera_start: Transform3D = scene.camera.transform
	await scene.settle(240)
	await scene.shot("still")
	for i in 16:
		await scene.settle(1)
		if "--capture-diffuse-values" in OS.get_cmdline_user_args():
			await scene.shot("temporal_%02d" % i)
		scene.get_viewport().get_texture().get_image().save_png(scene.output.path_join("still_%02d.png" % i))
	await scene.settle(704)
	await scene.shot("converged")
	# Bounded relighting and camera-only motion are independent experiments.
	for i in 180:
		scene._set_tod(0.25 + float(i) / 180.0 * 0.2)
		await scene.settle(1)
	await scene.shot("sunset")
	scene._set_tod(0.25)
	await scene.settle(64)
	for i in 180:
		scene.camera.position = Vector3(-8 + 16 * i / 180.0, 2.2, 0.0)
		scene.camera.look_at(scene.camera.position + Vector3(cos(i * 0.03), 0.1, sin(i * 0.03)))
		await scene.settle(1)
		if i % 45 == 0: await scene.shot("motion_%03d" % i)
	await scene.shot("motion_end")
	scene.camera.transform = camera_start
	await scene.settle(64)
	await scene.shot("returned")
	# Exercise partial workgroups, cache recreation and disabled-GI output.
	var original_size := root.size
	scene.resize_acceptance(Vector2i(961, 541))
	await scene.settle(96)
	await scene.shot("odd_resize")
	scene.resize_acceptance(original_size)
	await scene.settle(96)
	await scene.shot("resize_restored")
	var stats: Dictionary = scene.gi.get_statistics()
	assert(not stats.nrd_active and stats.specular_rays == 0)
	assert(stats.local_lights == 0 and stats.emissive_triangles == 0)
	print("[DIFFUSE_FOCUS] completed")
	quit()
