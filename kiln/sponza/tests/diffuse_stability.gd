extends SceneTree
## Real-window diffuse-GI stability sequence.
## Saves the displayed indirect diffuse every rendered frame without capture
## readbacks changing the cadence. TOD and camera motion are measured separately.

var scene: Node

func _initialize() -> void:
	ProjectSettings.set_setting("rendering/kiln/nrd", false)
	ProjectSettings.set_setting("rendering/kiln/surfel_specular", false)
	call_deferred("run")

func save_frame(directory: String, frame: int) -> void:
	root.get_texture().get_image().save_png(directory.path_join("%04d.png" % frame))

func run() -> void:
	scene = load("res://sponza.tscn").instantiate()
	root.add_child(scene)
	current_scene = scene
	assert(scene.output != "")
	root.set_flag(Window.FLAG_ALWAYS_ON_TOP, true)
	scene.controls.hide()
	scene.animate = false
	scene.orbit = false
	scene._set_debug_view(25)
	scene._set_tod(0.25)
	await scene.settle(240)

	var tod_directory: String = scene.output.path_join("tod_frames")
	var motion_directory: String = scene.output.path_join("motion_frames")
	DirAccess.make_dir_recursive_absolute(tod_directory)
	DirAccess.make_dir_recursive_absolute(motion_directory)
	var statistics := {"tod": [], "motion": []}

	# One hour over 120 frames is intentionally faster than the demo timeline.
	# A stable estimator should follow it as a smooth signal, not as flashing disks.
	for frame in 120:
		scene._set_tod(0.25 + float(frame + 1) / 2880.0)
		if frame in [0, 59, 119]:
			scene.gi.request_capture(scene.output.path_join("tod_%04d" % frame))
		await scene.settle(1)
		save_frame(tod_directory, frame)
		var stats: Dictionary = scene.gi.get_statistics()
		statistics.tod.append({"frame": frame, "response": stats.lighting_response})

	# Return to a settled noon cache, then expose previously unseen receivers with
	# a deterministic translation and turn. Every rendered frame is retained.
	scene._set_tod(0.25)
	await scene.settle(96)
	for frame in 120:
		var t := float(frame + 1) / 120.0
		scene.camera.position = Vector3(-10.0 + 16.0 * t, 2.2, sin(t * PI) * 1.5)
		scene.camera.look_at(Vector3(5.0, 2.8, sin(t * PI * 1.4) * 2.0))
		if frame in [0, 29, 59, 89, 119]:
			scene.gi.request_capture(scene.output.path_join("motion_%04d" % frame))
		await scene.settle(1)
		save_frame(motion_directory, frame)
		var stats: Dictionary = scene.gi.get_statistics()
		statistics.motion.append({"frame": frame, "response": stats.lighting_response})

	FileAccess.open(scene.output.path_join("stability.json"), FileAccess.WRITE).store_string(JSON.stringify(statistics, "\t"))
	print("[DIFFUSE_STABILITY] completed")
	quit()
