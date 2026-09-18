extends SceneTree
## Measure cold-cache diffuse values before the previous 240-frame warm-up.
## Synchronous readbacks are diagnostics, not frame-time measurements.

func _initialize() -> void:
	ProjectSettings.set_setting("rendering/kiln/surfel_specular", false)
	call_deferred("run")

func run() -> void:
	var scene = load("res://sponza.tscn").instantiate()
	root.add_child(scene)
	current_scene = scene
	assert(scene.output != "")
	assert(not ProjectSettings.get_setting("rendering/kiln/nrd"))
	root.set_flag(Window.FLAG_ALWAYS_ON_TOP, true)
	scene.controls.hide()
	scene.animate = false
	scene.orbit = false
	scene.time = 0.0
	scene._set_tod(0.25)
	scene._set_debug_view(25)
	var revisions: Array[Dictionary] = []
	# Request before drawing the numbered frame. metadata.frame is zero-based.
	for frame in range(1, 513):
		if frame in [1, 4, 8, 16, 32, 64, 128, 240, 512]:
			scene.gi.request_capture(scene.output.path_join("cold_%04d" % frame))
		await scene.settle(1)
		if frame in [1, 4, 8, 16, 32, 64, 128, 240, 512]:
			var stats: Dictionary = scene.gi.get_statistics()
			revisions.append({"frame": frame, "material_version": stats.material_version, "moving": stats.moving})
	FileAccess.open(scene.output.path_join("startup_state.json"), FileAccess.WRITE).store_string(JSON.stringify(revisions, "\t"))
	if "--capture-temporal" in OS.get_cmdline_user_args():
		for index in 16:
			scene.gi.request_capture(scene.output.path_join("temporal_%02d" % index))
			await scene.settle(1)
	assert(not scene.gi.get_statistics().nrd_active)
	print("[DIFFUSE_STARTUP] completed")
	quit()
