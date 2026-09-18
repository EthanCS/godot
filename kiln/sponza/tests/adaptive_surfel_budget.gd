extends SceneTree
## Validate adaptive surfel-ray budget states through the public GI statistics.

var scene: Node

func _initialize() -> void:
	ProjectSettings.set_setting("rendering/kiln/nrd", false)
	ProjectSettings.set_setting("rendering/kiln/surfel_specular", false)
	ProjectSettings.set_setting("rendering/kiln/surfel_ray_budget", 2097152)
	ProjectSettings.set_setting("rendering/kiln/surfel_adaptive_budget", true)
	call_deferred("run")

func check(expected_budget: int, expected_mode: String) -> void:
	var stats: Dictionary = scene.gi.get_statistics()
	assert(stats.surfel_ray_budget == expected_budget, "budget %s != %s" % [stats.surfel_ray_budget, expected_budget])
	assert(stats.surfel_ray_budget_mode == expected_mode, "mode %s != %s" % [stats.surfel_ray_budget_mode, expected_mode])

func run() -> void:
	scene = load("res://sponza.tscn").instantiate()
	root.add_child(scene)
	current_scene = scene
	scene.controls.hide()
	scene.animate = false
	scene.orbit = false
	scene._set_tod(0.25)
	await scene.settle(1)
	check(1048576, "bootstrap")
	await scene.settle(20)
	check(393216, "stationary")

	scene.camera.position.x += 0.1
	await scene.settle(1)
	check(524288, "motion")

	scene._set_tod(0.45)
	await scene.settle(1)
	var relight: Dictionary = scene.gi.get_statistics()
	assert(relight.surfel_ray_budget > 524288)
	assert(relight.surfel_ray_budget <= 2097152)
	assert(relight.surfel_ray_budget_mode == "relighting")

	ProjectSettings.set_setting("rendering/kiln/surfel_ray_budget", 262144)
	await scene.settle(1)
	check(262144, "configured_cap")

	ProjectSettings.set_setting("rendering/kiln/surfel_adaptive_budget", false)
	ProjectSettings.set_setting("rendering/kiln/surfel_ray_budget", 700000)
	await scene.settle(1)
	check(700000, "fixed")

	ProjectSettings.set_setting("rendering/kiln/surfel_adaptive_budget", true)
	ProjectSettings.set_setting("rendering/kiln/surfel_ray_budget", 0)
	await scene.settle(1)
	check(0, "uncapped")
	print("[ADAPTIVE_SURFEL_BUDGET] passed")
	quit()
