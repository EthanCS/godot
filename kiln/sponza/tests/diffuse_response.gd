extends SceneTree
## Verify raw diffuse response after neighbor-history reuse, in a real window.

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
	scene._set_tod(0.25)
	await scene.settle(320)
	await scene.shot("day")
	scene.sun.light_energy = 0.0
	scene.gi.set_lighting(Vector3.UP, Vector3.ONE, 0, 0, 0)
	await scene.settle(8)
	await scene.shot("off_8")
	await scene.settle(22)
	await scene.shot("off_32")
	await scene.settle(94)
	await scene.shot("off_128")
	scene._set_tod(0.25)
	await scene.settle(32)
	await scene.shot("restored_32")
	await scene.settle(94)
	await scene.shot("restored_128")
	scene._set_debug_view(25)
	await scene.settle(16)
	await scene.shot("linear_view")
	# Loading textures must not cause a false material revision. Genuine edits
	# must still invalidate transport and activate the relighting estimator.
	var original_color: Color = scene.floor_material.albedo_color
	var material_version: int = scene.gi.get_statistics().material_version
	scene.floor_material.albedo_color = original_color * Color(0.5, 0.25, 0.125, 1.0)
	await scene.settle(4)
	assert(scene.gi.get_statistics().material_version > material_version)
	assert(scene.gi.get_statistics().moving)
	material_version = scene.gi.get_statistics().material_version
	# Relighting now keeps a 64-frame sampling window for filtered catch-up.
	await scene.settle(72)
	assert(scene.gi.get_statistics().material_version == material_version)
	assert(not scene.gi.get_statistics().moving)
	scene.floor_material.albedo_color = original_color
	await scene.settle(4)
	assert(scene.gi.get_statistics().material_version > material_version)
	assert(scene.gi.get_statistics().moving)
	print("[DIFFUSE_RESPONSE] material update checks passed")
	assert(not scene.gi.get_statistics().nrd_active)
	print("[DIFFUSE_RESPONSE] completed")
	quit()
