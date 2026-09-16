extends SceneTree
# Three complete scene lifetimes through one viewport; require a memory plateau.
func _initialize() -> void:
	call_deferred("run")
func wait_frames(count: int) -> void:
	for i in count:
		await process_frame
		await RenderingServer.frame_post_draw
func run() -> void:
	root.size = Vector2i(1001, 703)
	root.use_taa = false
	var records := []
	for cycle in 3:
		var scene: Node3D = load("res://island.tscn").instantiate()
		root.add_child(scene)
		current_scene = scene
		scene.fixed_view = cycle
		scene.replay_time = 4.0
		scene.hud.visible = false
		await wait_frames(90)
		var stats: Dictionary = scene.gi.get_statistics()
		if cycle == 0:
			stats["bvh_validation"] = scene.gi.validate_bvh(64)
			assert(stats.bvh_validation.passed)
		stats["video_memory"] = Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED)
		stats["objects"] = Performance.get_monitor(Performance.OBJECT_COUNT)
		records.append(stats)
		current_scene = null
		scene.queue_free()
		await wait_frames(8)
		print("[KILN_RELOAD] cycle ", cycle, " video memory ", stats.video_memory)
	var passed: bool = records[2].video_memory <= records[1].video_memory * 1.02 + 8388608 and records[2].objects <= records[1].objects + 32
	FileAccess.open("/tmp/kiln-reload.json", FileAccess.WRITE).store_string(JSON.stringify({"passed": passed, "cycles": records}, "\t"))
	quit(0 if passed else 1)
