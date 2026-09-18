extends SceneTree
## Sun/sky diffuse + reflections; command-line flags select the denoiser/ray layout.

func _initialize() -> void:
	ProjectSettings.set_setting("rendering/kiln/surfel_specular", true)
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
	scene._set_tod(0.25)
	scene.camera.position = Vector3(-9, 1.6, 0.4)
	scene.camera.look_at(Vector3(4, 1.0, 0))
	var camera_start: Transform3D = scene.camera.transform
	for roughness in [0.06, 0.18, 0.45, 0.85]:
		var label := "r%02d" % roundi(roughness * 100)
		scene.floor_material.roughness = roughness
		scene._set_debug_view(0)
		await scene.settle(160)
		await scene.shot(label + "_lit")
		scene._set_debug_view(28)
		await scene.settle(8)
		for i in 8:
			await scene.shot(label + "_noise_%02d" % i)
		for i in 48:
			scene.camera.position = camera_start.origin + Vector3(0, 0, sin(float(i) / 47.0 * PI) * 0.6)
			scene.camera.look_at(Vector3(4, 1.0, 0))
			await scene.settle(1)
			if i == 23: await scene.shot(label + "_motion")
		scene.camera.transform = camera_start
		await scene.settle(8)
		await scene.shot(label + "_settled8")
		await scene.settle(88)
		await scene.shot(label + "_settled96")
	# Material demodulation must preserve both dielectric extinction and metal tint.
	scene.floor_material.roughness = 0.18
	scene.floor_material.metallic_specular = 0.0
	await scene.settle(64)
	await scene.shot("zero_f0")
	scene.floor_material.metallic_specular = 0.5
	scene.floor_material.metallic = 1.0
	await scene.settle(64)
	await scene.shot("metal_floor")
	scene.floor_material.metallic = 0.0
	var stats: Dictionary = scene.gi.get_statistics()
	assert(stats.local_lights == 0 and stats.emissive_triangles == 0 and stats.nrd_diffuse_rays == 0)
	assert(stats.specular_rays == 2)
	var frames: int = int(stats.rendered_frames)
	ProjectSettings.set_setting("rendering/kiln/surfel_specular", false)
	await scene.settle(16)
	await scene.shot("specular_off")
	assert(scene.gi.get_statistics().specular_rays == 0)
	ProjectSettings.set_setting("rendering/kiln/surfel_specular", true)
	await scene.settle(48)
	assert(int(scene.gi.get_statistics().rendered_frames) > frames)
	await scene.shot("specular_on_again")
	scene.resize_acceptance(Vector2i(961, 541))
	await scene.settle(64)
	await scene.shot("odd_resize")
	scene.gi.set_enabled(false)
	await scene.settle(2)
	await scene.shot("gi_off")
	print("[SPECULAR_FOCUS] completed")
	quit()
