extends SceneTree
## Exercise specialization changes and cached-pipeline reuse on a real GPU.
## Pass --still --output=<temporary directory>; --software also tests compute BVH.

func _initialize() -> void:
	call_deferred("run")

func run() -> void:
	var scene = load("res://sponza.tscn").instantiate()
	root.add_child(scene)
	current_scene = scene
	assert(scene.output != "")
	scene.animate = false
	scene.resize_acceptance(Vector2i(481, 271))
	root.set_flag(Window.FLAG_ALWAYS_ON_TOP, true)
	for rays in [2, 1, 3, 8, 2]:
		ProjectSettings.set_setting("rendering/kiln/specular_rays", rays)
		await scene.settle(96)
		var label := "rays_%d_frame_%d" % [rays, scene.ticks]
		await scene.shot(label)
		var directory: String = scene.output.path_join(label)
		var metadata: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(directory.path_join("metadata.json")))
		assert(metadata.specular_rays == rays)
		assert(metadata.width == 481 and metadata.height == 271)
		for channel in ["specular", "fresnel"]:
			var image := Image.create_from_data(481, 271, false, Image.FORMAT_RGBAH, FileAccess.get_file_as_bytes(directory.path_join(channel + ".bin")))
			image.convert(Image.FORMAT_RGBAF)
			var peak := 0.0
			for value in image.get_data().to_float32_array():
				assert(is_finite(value) and value >= 0.0)
				peak = maxf(peak, value)
			assert(peak > 0.0001)
	print("[SPECULAR_RAY_COUNTS] passed 2 -> 1 -> 3 -> 8 -> 2 at 481x271")
	quit()
