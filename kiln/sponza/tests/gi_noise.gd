extends SceneTree
## Real-window noise regression: identical still and continuously changing TOD.
## PNG sequences exclude the HUD; GI readbacks are taken after each sequence.

func _initialize() -> void:
	call_deferred("run")

func run() -> void:
	var scene = load("res://sponza.tscn").instantiate()
	root.add_child(scene)
	current_scene = scene
	assert(scene.output != "")
	root.set_flag(Window.FLAG_ALWAYS_ON_TOP, true)
	scene.controls.visible = false
	scene.orbit = false
	scene.animate = false
	var initial_camera: Transform3D = scene.camera.transform
	for mode in ["still", "tod", "moving"]:
		scene.animate = false
		scene.camera.transform = initial_camera
		scene.time = 0.0
		scene._set_tod(0.25)
		scene._set_debug_view(0)
		scene.gi.reset_history()
		await scene.settle(192)
		scene.orbit = mode == "moving"
		scene.animate = mode != "still"
		await scene.settle(32)
		for sample in 16:
			await scene.settle(1)
			scene.get_viewport().get_texture().get_image().save_png(scene.output.path_join("%s_%02d.png" % [mode, sample]))
		await scene.shot(mode)
		scene.animate = false
		scene._set_debug_view(28)
		await scene.settle(2)
		scene.get_viewport().get_texture().get_image().save_png(scene.output.path_join(mode + "_reflection.png"))
	print("[GI_NOISE] completed")
	quit()
