extends SceneTree
## Upper-floor disocclusion regression for bounded surfel lookup. Build a mature
## ground-floor cache, jump into two previously unseen corridors, and retain the
## first 120 frames so a one-frame black 8x8 allocation pattern cannot be hidden
## by waiting for the cache to settle.

var scene: Node

func _initialize() -> void:
	ProjectSettings.set_setting("rendering/kiln/nrd", false)
	ProjectSettings.set_setting("rendering/kiln/surfel_specular", false)
	call_deferred("run")

func save_frame(directory: String, frame: int) -> void:
	root.get_texture().get_image().save_png(directory.path_join("%04d.png" % frame))

func capture_sequence(directory: String, debug_view: int) -> Array:
	scene._set_debug_view(debug_view)
	var statistics := []
	for frame in 120:
		if frame in [0, 59, 119]:
			scene.gi.request_capture(scene.output.path_join("%s_%04d" % [directory.get_file(), frame]))
		await scene.settle(1)
		save_frame(directory, frame)
		var stats: Dictionary = scene.gi.get_statistics()
		statistics.append({"frame": frame, "response": stats.lighting_response})
	return statistics

func run() -> void:
	scene = load("res://sponza.tscn").instantiate()
	root.add_child(scene)
	current_scene = scene
	assert(scene.output != "")
	root.set_flag(Window.FLAG_ALWAYS_ON_TOP, true)
	scene.controls.hide()
	scene.animate = false
	scene.orbit = false
	scene._set_tod(0.25)
	# Build a mature cache from the normal ground-floor view first. The regression
	# happens on the first frames after entering the upper corridor, not after the
	# newly exposed receivers have been allowed to converge there for seconds.
	await scene.settle(240)

	var raw_directory: String = scene.output.path_join("raw_frames")
	var color_directory: String = scene.output.path_join("color_frames")
	DirAccess.make_dir_recursive_absolute(raw_directory)
	DirAccess.make_dir_recursive_absolute(color_directory)
	# Stand inside one upper side corridor and look outward into the courtyard.
	# Looking along the central gallery axis does not exercise the dense stack of
	# corridor wall, ceiling, floor and opposite-facing surfaces.
	scene.camera.position = Vector3(-4.0, 5.2, 5.4)
	scene.camera.look_at(Vector3(-4.0, 4.8, 0.0))
	var raw_statistics: Array = await capture_sequence(raw_directory, 25)

	# Move to the previously unseen mirrored corridor for the composed sequence,
	# so its first frames test disocclusion instead of an already settled cache.
	scene.camera.position = Vector3(-4.0, 5.2, -5.4)
	scene.camera.look_at(Vector3(-4.0, 4.8, 0.0))
	var statistics := {
		"raw": raw_statistics,
		"color": await capture_sequence(color_directory, 0),
	}
	FileAccess.open(scene.output.path_join("upstairs_stability.json"), FileAccess.WRITE).store_string(JSON.stringify(statistics, "\t"))
	print("[UPSTAIRS_STABILITY] completed")
	quit()
