extends SceneTree
## Real-window regression: continuous TOD, lighting steps and cache-preserving resize.
## Capture each selected rendered frame, without introducing extra settle frames.

var scene: Node

func _initialize() -> void:
	ProjectSettings.set_setting("rendering/kiln/surfel_specular", false)
	call_deferred("run")

func frames(label: String, count: int, captures: Array, tod := false) -> void:
	for frame in range(1, count + 1):
		if tod:
			scene._set_tod(0.25 + float(frame) / 2700.0)
		if frame in captures:
			scene.gi.request_capture(scene.output.path_join("%s_%04d" % [label, frame]))
		await scene.settle(1)
		if frame in captures:
			print("[DIFFUSE_RELIGHT] ", label, " ", frame)

func run() -> void:
	scene = load("res://sponza.tscn").instantiate()
	root.add_child(scene)
	current_scene = scene
	assert(scene.output != "")
	assert(not ProjectSettings.get_setting("rendering/kiln/nrd"))
	root.set_flag(Window.FLAG_ALWAYS_ON_TOP, true)
	scene.controls.hide()
	scene.animate = false
	scene.orbit = false
	scene._set_debug_view(25)
	if "--cache-lifecycle" in OS.get_cmdline_user_args():
		scene._set_tod(0.25)
		await scene.settle(128)
		await frames("cached", 1, [1])
		var previous_frames: int = scene.gi.get_statistics().rendered_frames
		scene.resize_acceptance(Vector2i(1280, 720))
		await frames("grown", 1, [1])
		assert(scene.gi.get_statistics().rendered_frames == previous_frames + 1)
		scene.gi.reset_history()
		await frames("explicit_reset", 1, [1])
		assert(scene.gi.get_statistics().rendered_frames == 1)
		# TAA reconfiguration must preserve the world cache too. Exercise both
		# diffuse and actual GGX reflection resources in the final shaded output.
		ProjectSettings.set_setting("rendering/kiln/surfel_specular", true)
		scene._set_debug_view(0)
		await scene.settle(64)
		previous_frames = scene.gi.get_statistics().rendered_frames
		scene.resize_acceptance(Vector2i(641, 361))
		await frames("lit_resize", 1, [1])
		assert(scene.gi.get_statistics().rendered_frames == previous_frames + 1)
		root.get_texture().get_image().save_png(scene.output.path_join("lit_resize.png"))
		await frames("lit_settled", 32, [32])
		root.get_texture().get_image().save_png(scene.output.path_join("lit_settled.png"))
		print("[DIFFUSE_RELIGHT] cache lifecycle passed")
		quit()
		return
	if "--tod-cycle" in OS.get_cmdline_user_args():
		scene._set_tod(0.25)
		await scene.settle(320)
		await frames("cycle", 2700, [1, 675, 1350, 2025, 2700], true)
		print("[DIFFUSE_RELIGHT] full TOD cycle completed")
		quit()
		return
	if "--tod-reference" in OS.get_cmdline_user_args():
		scene._set_tod(0.25 + 180.0 / 2700.0)
		await scene.settle(512)
		await frames("tod_reference", 1, [1])
		quit()
		return
	scene._set_tod(0.25)
	await scene.settle(320)
	await frames("static", 16, range(1, 17))
	await frames("tod", 180, range(165, 181), true)
	scene._set_tod(0.36)
	await frames("step", 256, [1, 2, 4, 8, 16, 32, 64, 128, 256])
	scene.resize_acceptance(Vector2i(641, 361))
	await frames("small", 32, [1, 2, 4, 8, 32])
	scene.resize_acceptance(Vector2i(1280, 720))
	await frames("large", 128, [1, 2, 4, 8, 32, 128])
	scene.sun.light_energy = 0.0
	scene.gi.set_lighting(Vector3.UP, Vector3.ONE, 0, 0, 0)
	await frames("off", 128, [8, 32, 64, 128])
	scene._set_tod(0.36)
	await frames("restore", 256, [8, 32, 64, 128, 256])
	print("[DIFFUSE_RELIGHT] completed")
	quit()
