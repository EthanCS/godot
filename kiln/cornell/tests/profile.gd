extends SceneTree
## Real-GPU per-pass diagnostic. Profiling is separate from throughput runs.
## --script res://tests/profile.gd -- --size=1920x1080 --output=<temporary-directory>

func _initialize() -> void:
	call_deferred("run")

func run() -> void:
	var scene = load("res://cornell.tscn").instantiate()
	root.add_child(scene)
	current_scene = scene
	assert(scene.frames == 0, "Do not enable capture while profiling")
	assert(scene.output != "", "Pass a temporary --output directory")
	root.set_flag(Window.FLAG_ALWAYS_ON_TOP, true)
	scene.gi.set_profiling(true)
	await scene.settle(600)
	var samples: Array = []
	var last_profile_frame := -1
	for frame in 300:
		await scene.settle(1)
		var stats: Dictionary = scene.gi.get_statistics()
		assert(stats.gpu_timestamps_available, "Real GPU timestamps are required")
		var profile_frame: int = stats.profile_frame
		if profile_frame != last_profile_frame:
			samples.append({"frame": profile_frame, "profile": stats.profile})
			last_profile_frame = profile_frame
	assert(samples.size() >= 290, "GPU frame profiling did not advance")
	var result := {"profiles": samples, "statistics": scene.gi.get_statistics(), "size": root.size}
	FileAccess.open(scene.output.path_join("profile.json"), FileAccess.WRITE).store_string(JSON.stringify(result))
	print("[CORNELL_PROFILE] ", scene.output, " samples=", samples.size())
	quit()
