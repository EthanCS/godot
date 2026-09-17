extends SceneTree
## Real-window NRD activation, fallback, camera cut and recreation checks.

func _initialize() -> void:
	call_deferred("run")

func run() -> void:
	var scene = load("res://sponza.tscn").instantiate()
	root.add_child(scene)
	current_scene = scene
	scene.animate = false
	scene.orbit = false
	scene.controls.hide()
	for enabled in [true, false, true]:
		ProjectSettings.set_setting("rendering/kiln/nrd", enabled)
		await scene.settle(64)
		assert(scene.gi.get_statistics().get("nrd_active", false) == enabled)
		await scene.shot("nrd_on" if enabled else "fallback")
	scene.camera.position += Vector3(4, 0, 2)
	scene.gi.reset_history()
	await scene.settle(32)
	await scene.shot("camera_cut")
	await scene.resize_acceptance(Vector2i(641, 361))
	await scene.settle(64)
	assert(scene.gi.get_statistics().get("nrd_active", false))
	assert(scene.gi.get_statistics().get("nrd_version", "") == "4.17.3")
	await scene.shot("odd_resize")
	print("[NRD_LIFECYCLE] passed")
	quit()
