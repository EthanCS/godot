extends Node3D
## Deterministic cornell box workload matching kajiya's `--scene cornell_box` default view,
## for the kajiya restir-meets-surfel parity comparison.
##
## kajiya references:
##   camera (0, 1, 8), identity rotation, vertical fov 52
##   cornell_box instance at (0, -1, 0), baked at scale 2.0
##   336_lrm car at (0, -0.01, 0), baked at scale 0.01 (skip with --no-car)
##   sun direction = spherical_to_cartesian(theta, phi) = (sin(phi)cos(theta), cos(phi), sin(phi)sin(theta))
const DEFAULT_SUN_THETA := -4.54
const DEFAULT_SUN_PHI := 1.48

var gi: KilnGIWorld
var camera: Camera3D
var sun: DirectionalLight3D
var environment: Environment
var sky_material: ProceduralSkyMaterial
var ticks := 0
var frames := 0
var output := ""
var sun_theta := DEFAULT_SUN_THETA
var sun_phi := DEFAULT_SUN_PHI
var with_car := true


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--sun-theta="): sun_theta = float(arg.get_slice("=", 1))
		if arg.begins_with("--sun-phi="): sun_phi = float(arg.get_slice("=", 1))
		if arg.begins_with("--frames="): frames = int(arg.get_slice("=", 1))
		if arg.begins_with("--output="): output = arg.trim_prefix("--output=")
		if arg == "--no-car": with_car = false
		if arg.begins_with("--size="):
			var size := arg.get_slice("=", 1).split("x")
			get_window().size = Vector2i(int(size[0]), int(size[1]))
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	if output != "":
		DirAccess.make_dir_recursive_absolute(output)

	var cornell_scene: PackedScene = load("res://assets/cornell_box/scene.gltf")
	assert(cornell_scene != null, "Run python kiln/tools/fetch_cornell.py before importing this project.")
	var cornell := cornell_scene.instantiate()
	cornell.scale = Vector3.ONE * 2.0
	cornell.position = Vector3(0, -1, 0)
	add_child(cornell)

	if with_car:
		var car_scene: PackedScene = load("res://assets/car/scene.gltf")
		var car := car_scene.instantiate()
		car.scale = Vector3.ONE * 0.01
		car.position = Vector3(0, -0.01, 0)
		add_child(car)

	environment = Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	sky_material = ProceduralSkyMaterial.new()
	var sky := Sky.new()
	sky.sky_material = sky_material
	environment.sky = sky
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)

	camera = Camera3D.new()
	camera.position = Vector3(0, 1, 8)
	camera.fov = 52.0
	camera.near = 0.1
	add_child(camera)
	camera.make_current()

	sun = DirectionalLight3D.new()
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 40
	add_child(sun)

	gi = KilnGIWorld.new()
	gi.environment = environment
	gi.set_quality(2, 256)
	add_child(gi)

	_set_sun(sun_theta, sun_phi)
	if frames > 0:
		_run_capture.call_deferred()

func _set_sun(theta: float, phi: float) -> void:
	# kajiya view/src/main.rs SunState::direction()
	var direction := Vector3(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta))
	sun.look_at_from_position(Vector3.ZERO, -direction)
	sun.light_energy = 1.0
	sun.light_color = Color(1, 1, 1)
	sky_material.sky_top_color = Color(0.23, 0.38, 0.62)
	sky_material.sky_horizon_color = Color(0.55, 0.61, 0.72)
	sky_material.ground_bottom_color = Color(0.12, 0.1, 0.08)
	sky_material.ground_horizon_color = sky_material.sky_horizon_color
	gi.set_sky(Vector3(0.26, 0.33, 0.46), Vector3(0.04, 0.12, 0.34), false)
	gi.set_lighting(direction, Vector3.ONE, 1.0, 1.0, 0.0)

func settle(count: int) -> void:
	for i in count:
		await get_tree().process_frame
		await RenderingServer.frame_post_draw

func _run_capture() -> void:
	await settle(frames)
	if output != "":
		get_viewport().get_texture().get_image().save_png(output.path_join("color.png"))
		FileAccess.open(output.path_join("lighting.json"), FileAccess.WRITE).store_string(JSON.stringify({
			"sun_theta": sun_theta,
			"sun_phi": sun_phi,
			"sun_direction": [sun_theta, sun_phi],
			"camera": [0, 1, 8],
			"fov": 52.0,
			"frames": frames,
			"with_car": with_car,
			"statistics": gi.get_statistics(),
		}))
		print("[CORNELL_CAPTURE] ", output)
	get_tree().quit()
