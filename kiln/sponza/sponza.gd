extends Node3D
## Deterministic Sponza GI workload. Everything here is independent of the island.
var gi: KilnGIWorld
var camera: Camera3D
var sun: DirectionalLight3D
var environment: Environment
var sky_material: ProceduralSkyMaterial
var emitters: Array[MeshInstance3D] = []
var lights: Array[Light3D] = []
var hud: Label
var ticks := 0
var time := 0.0
var animate := true
var duration := 0
var output := ""
var suite := false
var surfel_suite := false
var floor_material: StandardMaterial3D
var floor_roughness := 0.18
var specular_suite := false
var no_gi := false
var no_taa := false
var free_camera := false
const DEBUG_VIEWS = {
    0: ["Lit", "Final shaded scene. F4 / F5 browse Surfel views; F6 resets the cache."],
    7: ["Composed indirect", "Indirect diffuse and glossy reflection with material reflectance."],
    15: ["Surfel IDs", "Each color is a persistent GPU slot. Dark gaps separate display disks; use Coverage to find GI holes."],
    16: ["Surfel irradiance", "Cached diffuse irradiance / PI on each disk. Display: 1 - exp(-value * gain)."],
    17: ["Surfel normals", "World-space cached normal: RGB = XYZ * 0.5 + 0.5."],
    18: ["Surfel radius", "Support radius in world units. Blue 0.12 m -> green 0.41 m -> red 0.70 m."],
    19: ["Surfel age", "Frames since allocation. Blue 0 -> green 120 -> red 240+."],
    20: ["Surfel samples", "Accumulated rays (log scale). Blue 0 -> green 31 -> red 1024+."],
    21: ["Surfel updates", "Green: allocated < 4 frames ago. Orange: traced this frame. Blue: reused cache."],
    22: ["Surfel coverage", "Red: no support. Yellow: below 0.65 allocation target. Green: covered. Blue: weight sum >= 2."],
    23: ["Surfel contributors", "Compatible neighbors per pixel. Blue 1 -> green 16 -> red 32+. Magenta: none."],
    24: ["Surfel variance", "MSME relative deviation: sqrt(variance) / max(short mean, 0.01). Blue 0 -> green 2 -> red 4+."],
    25: ["Raw GI gather", "Full-resolution cache gather before temporal resolve, material reflectance and AO. Uses display gain."],
    26: ["Surfel grid levels", "Dominant surfel cell: blue 0.25 m / green 0.50 m / orange 1 m. Lines show world-grid boundaries."],
    27: ["Surfel anchors", "Blue: original static triangles. Orange: original dynamic triangles. No simplified proxy mesh."],
    13: ["Instance IDs", "G-buffer draw-instance IDs."],
    14: ["Geometric normals", "G-buffer geometric normals used by Surfel GI."],
    28: ["Indirect specular", "World-space GGX reflection rays, with Fresnel and multiple-scattering compensation."],
    1: ["Albedo", "Material albedo."],
    2: ["Shading normals", "Material shading normals, including normal maps."],
    8: ["Ambient occlusion", "Independent XeGTAO visibility."],
    3: ["Roughness", "Material perceptual roughness."],
    4: ["Emission", "Authored material emission."],
    5: ["Material channels", "Specular, occlusion and material data."],
    6: ["Depth", "Scene depth."],
    9: ["Motion", "Material motion vectors."],
    10: ["Direct lighting", "Direct lighting without GI."],
    11: ["Cluster light count", "Number of lights in the visible cluster."],
    12: ["GI coverage grayscale", "Clamped Surfel support weight. Use Surfel Coverage for the full heat map."]
}
var debug_mode := 0
var surfel_debug_suite := false
var debug_selector: OptionButton
var debug_description: Label
var debug_panel: PanelContainer
var orbit := true
var ray_count := 2
var benchmark := false
var last_usec := 0
var frame_ms: Array[float] = []
var query_backend := 0
var local_mode := false
var tod_suite := false
var tod_video := false
var tod_phase := 0.25
var start_hour := 12.0
var tod_slider: HSlider
var controls: CanvasLayer
var sky_energy_value := 0.0


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg == "--surfel-two-bounce": ProjectSettings.set_setting("rendering/kiln/surfel_multibounce", false)
		if arg == "--dry-floor": floor_roughness = 0.85
		if arg.begins_with("--roughness="): floor_roughness = clampf(float(arg.get_slice("=", 1)), 0.02, 1.0)
		if arg == "--specular-suite": specular_suite = true
		if arg == "--no-specular": ProjectSettings.set_setting("rendering/kiln/surfel_specular", false)
		if arg == "--full-specular-rate": ProjectSettings.set_setting("rendering/kiln/specular_checkerboard", false)
		if arg.begins_with("--specular-rays="): ProjectSettings.set_setting("rendering/kiln/specular_rays", int(arg.get_slice("=", 1)))
		if arg == "--suite": suite = true
		if arg == "--surfel-suite": surfel_suite = true
		if arg == "--surfel-debug-suite": surfel_debug_suite = true
		if arg.begins_with("--debug-view="): debug_mode = int(arg.get_slice("=", 1))
		if arg == "--multi-light": local_mode = true
		if arg == "--software": query_backend = 1
		if arg == "--hardware": query_backend = 2
		if arg == "--tod-suite": tod_suite = true
		if arg == "--tod-video": tod_video = true
		if arg.begins_with("--hour="): start_hour = float(arg.get_slice("=", 1))
		if arg == "--no-gi": no_gi = true
		if arg == "--no-aa": no_taa = true
		if arg == "--still": animate = false
		if arg == "--benchmark": benchmark = true
		if arg.begins_with("--frames="): duration = int(arg.get_slice("=", 1))
		if arg.begins_with("--output="): output = arg.trim_prefix("--output=")
		if arg.begins_with("--rays="): ray_count = int(arg.get_slice("=", 1))
		if arg.begins_with("--size="):
			var size := arg.get_slice("=", 1).split("x")
			get_window().size = Vector2i(int(size[0]), int(size[1]))
	get_viewport().use_taa = not no_taa
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	if output != "": DirAccess.make_dir_recursive_absolute(output)
	var mesh: Mesh = load("res://assets/sponza.obj")
	assert(mesh != null, "Run python kiln/tools/fetch_sponza.py before importing this project.")
	var building := MeshInstance3D.new()
	building.mesh = mesh
	for surface in mesh.get_surface_count():
		var source := mesh.surface_get_material(surface)
		if source is StandardMaterial3D and source.resource_name == "floor":
			floor_material = source.duplicate()
			floor_material.roughness = floor_roughness
			floor_material.metallic = 0.0
			floor_material.metallic_specular = 0.5
			floor_material.roughness_texture = null
			building.set_surface_override_material(surface, floor_material)
	assert(floor_material != null, "Sponza floor material must be present")
	building.scale = Vector3.ONE * 0.01
	add_child(building)
	print("[SPONZA] bounds=", mesh.get_aabb(), " surfaces=", mesh.get_surface_count())
	environment = Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	environment.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	sky_material = ProceduralSkyMaterial.new()
	sky_material.sky_curve = 0.5
	var sky := Sky.new()
	sky.sky_material = sky_material
	environment.sky = sky
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)
	camera = Camera3D.new()
	camera.position = Vector3(-10, 2.2, 0)
	camera.near = 0.1
	camera.far = 80
	camera.fov = 68
	add_child(camera)
	camera.look_at(Vector3(5, 3.0, 0))
	camera.make_current()
	sun = DirectionalLight3D.new()
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 65
	sun.shadow_normal_bias = 0.5
	add_child(sun)
	for i in 12:
		var light: Light3D = SpotLight3D.new() if i % 3 == 0 else OmniLight3D.new()
		light.position = Vector3(-10 + (i / 2) * 4, 2.8, -3.0 if i % 2 == 0 else 3.0)
		light.light_color = Color.from_hsv(float(i) / 12.0, 0.6, 1.0)
		light.light_energy = 4.0
		light.set_param(Light3D.PARAM_RANGE, 8.0)
		light.shadow_enabled = i < 4
		add_child(light)
		if light is SpotLight3D:
			light.look_at(light.position + Vector3(0, -1, 0.3))
		lights.append(light)
	for i in 3:
		var item := MeshInstance3D.new()
		var shape := BoxMesh.new()
		shape.size = Vector3(0.8, 1.6, 0.8)
		item.mesh = shape
		item.position = Vector3(-4 + i * 5, 0.8, -1.4)
		var material := StandardMaterial3D.new()
		material.albedo_color = Color(0.35, 0.35, 0.35)
		material.emission_enabled = true
		material.emission = Color.from_hsv(float(i) / 3.0, 0.85, 1.0)
		material.emission_energy_multiplier = 4.0
		item.material_override = material
		item.set_meta("kiln_dynamic", true)
		add_child(item)
		emitters.append(item)
	gi = KilnGIWorld.new()
	gi.environment = environment
	gi.set_query_backend(query_backend)
	gi.set_quality(ray_count, 256)
	gi.set_enabled(not no_gi)
	add_child(gi)
	controls = CanvasLayer.new()
	add_child(controls)
	hud = Label.new()
	hud.position = Vector2(14, 12)
	hud.add_theme_color_override("font_shadow_color", Color.BLACK)
	hud.add_theme_constant_override("shadow_offset_x", 1)
	hud.add_theme_constant_override("shadow_offset_y", 1)
	var hud_style := StyleBoxFlat.new()
	hud_style.bg_color = Color(0.025, 0.03, 0.045, 0.88)
	hud_style.content_margin_left = 8
	hud_style.content_margin_right = 8
	hud_style.content_margin_top = 6
	hud_style.content_margin_bottom = 6
	hud.add_theme_stylebox_override("normal", hud_style)
	controls.add_child(hud)
	var bar := HBoxContainer.new()
	bar.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_WIDE)
	bar.offset_left = 18
	bar.offset_right = -18
	bar.offset_top = -46
	bar.offset_bottom = -14
	controls.add_child(bar)
	var time_label := Label.new()
	time_label.text = "Time of day"
	bar.add_child(time_label)
	tod_slider = HSlider.new()
	tod_slider.min_value = 0.0
	tod_slider.max_value = 24.0
	tod_slider.step = 0.02
	tod_slider.custom_minimum_size.x = 360
	bar.add_child(tod_slider)
	tod_slider.value_changed.connect(func(hour: float):
		animate = false
		_set_tod(fposmod((hour - 6.0) / 24.0, 1.0)))
	var local_toggle := CheckButton.new()
	local_toggle.text = "Local lights + emitters"
	local_toggle.button_pressed = local_mode
	local_toggle.toggled.connect(_set_local_mode)
	bar.add_child(local_toggle)
	_build_debug_controls()
	if output != "" and not benchmark: controls.hide()
	_set_debug_view(debug_mode)
	_set_local_mode(local_mode)
	_set_tod(fposmod((start_hour - 6.0) / 24.0, 1.0))
	if tod_suite or tod_video:
		animate = false
		controls.hide()
		if tod_video: _run_tod_video.call_deferred()
		else: _run_tod_suite.call_deferred()
	if suite:
		animate = false
		controls.hide()
		_run_suite.call_deferred()
	if specular_suite:
		animate = false
		controls.hide()
		_run_specular_suite.call_deferred()
	if surfel_suite:
		animate = false
		controls.hide()
		_run_surfel_suite.call_deferred()

	if surfel_debug_suite:
		animate = false
		controls.hide()
		_run_surfel_debug_suite.call_deferred()

func _build_debug_controls() -> void:
	debug_panel = PanelContainer.new()
	debug_panel.set_anchors_and_offsets_preset(Control.PRESET_TOP_RIGHT)
	debug_panel.offset_left = -398
	debug_panel.offset_right = -14
	debug_panel.offset_top = 104
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.025, 0.03, 0.045, 0.94)
	style.content_margin_left = 12
	style.content_margin_right = 12
	style.content_margin_top = 10
	style.content_margin_bottom = 10
	debug_panel.add_theme_stylebox_override("panel", style)
	controls.add_child(debug_panel)
	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 8)
	debug_panel.add_child(column)
	debug_selector = OptionButton.new()
	for mode in DEBUG_VIEWS:
		debug_selector.add_item(DEBUG_VIEWS[mode][0], mode)
	debug_selector.item_selected.connect(func(index: int): _set_debug_view(debug_selector.get_item_id(index)))
	column.add_child(debug_selector)
	debug_description = Label.new()
	debug_description.custom_minimum_size.x = 354
	debug_description.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	debug_description.add_theme_font_size_override("font_size", 14)
	column.add_child(debug_description)
	for property in ["surfel_debug_radius", "surfel_debug_gain"]:
		var row := HBoxContainer.new()
		column.add_child(row)
		var label := Label.new()
		label.text = "Display disk scale" if property.ends_with("radius") else "GI display gain"
		label.custom_minimum_size.x = 142
		label.add_theme_font_size_override("font_size", 14)
		row.add_child(label)
		var slider := HSlider.new()
		var radius: bool = property.ends_with("radius")
		slider.min_value = 0.15 if radius else 0.1
		slider.max_value = 1.0 if radius else 16.0
		slider.step = 0.05 if radius else 0.1
		slider.value = ProjectSettings.get_setting("rendering/kiln/" + property, 0.45 if radius else 4.0)
		slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(slider)
		var value := Label.new()
		value.custom_minimum_size.x = 42
		value.text = "%.2f" % slider.value
		row.add_child(value)
		slider.value_changed.connect(func(number: float):
			ProjectSettings.set_setting("rendering/kiln/" + property, number)
			value.text = "%.2f" % number)
	var reset := Button.new()
	reset.text = "Reset Surfel cache (F6)"
	reset.pressed.connect(func(): gi.reset_history())
	column.add_child(reset)
	var note := Label.new()
	note.text = "F4 / F5 cycle   |   F7 final   |   F8 hide panel"
	note.add_theme_font_size_override("font_size", 13)
	column.add_child(note)

func _set_debug_view(mode: int) -> void:
	if not DEBUG_VIEWS.has(mode): mode = 0
	debug_mode = mode
	ProjectSettings.set_setting("rendering/kiln/debug_view", mode)
	# Categorical colors and individual disks must not accumulate TAA trails.
	get_viewport().use_taa = not no_taa and mode < 13
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR if mode >= 13 else Environment.TONE_MAPPER_FILMIC
	if is_instance_valid(debug_selector):
		debug_selector.select(debug_selector.get_item_index(mode))
		debug_description.text = DEBUG_VIEWS[mode][1]
		if RenderingServer.get_current_rendering_method() != "kiln_deferred":
			debug_description.text = "Debug views require Kiln Deferred. This renderer retains its normal output."

func _cycle_surfel_debug(step: int) -> void:
	var current := debug_mode if debug_mode >= 15 and debug_mode <= 27 else (14 if step > 0 else 28)
	_set_debug_view(15 + posmod(current - 15 + step, 13))

func _set_local_mode(enabled: bool) -> void:
	local_mode = enabled
	for light in lights: light.visible = enabled
	for emitter in emitters: emitter.visible = enabled

func _set_tod(phase: float) -> void:
	tod_phase = fposmod(phase, 1.0)
	if is_instance_valid(tod_slider): tod_slider.set_value_no_signal(fposmod(tod_phase * 24.0 + 6.0, 24.0))
	var elevation := sin(phase * TAU)
	var direction := Vector3(cos(phase * TAU), elevation, 0.35).normalized()
	var energy := maxf(elevation, 0.0) * 1.5
	var sky_energy := 0.04 + maxf(elevation, 0.0) * 0.7
	sky_energy_value = sky_energy
	var tint := Color(1.0, lerpf(0.45, 0.97, maxf(elevation, 0.0)), lerpf(0.2, 0.9, maxf(elevation, 0.0)))
	sun.look_at_from_position(Vector3.ZERO, -direction)
	sun.light_energy = energy
	sun.light_color = tint
	sky_material.sky_top_color = Color(0.23, 0.38, 0.62) * sky_energy
	sky_material.sky_horizon_color = Color(0.55, 0.61, 0.72) * sky_energy
	sky_material.ground_bottom_color = Color(0.12, 0.1, 0.08) * sky_energy
	sky_material.ground_horizon_color = sky_material.sky_horizon_color
	gi.set_sky(Vector3(0.26, 0.33, 0.46), Vector3(0.04, 0.12, 0.34), false)
	var c := tint.srgb_to_linear()
	gi.set_lighting(direction, Vector3(c.r, c.g, c.b), energy, sky_energy, phase)

func _process(_delta: float) -> void:
	if not is_instance_valid(gi): return
	var now := Time.get_ticks_usec()
	if benchmark and ticks >= 60 and last_usec > 0: frame_ms.append((now - last_usec) / 1000.0)
	last_usec = now
	if animate and not suite:
		time += 1.0 / 60.0 # Deterministic, and pause/resume never jumps.
		_set_tod(fposmod(tod_phase + 1.0 / (45.0 * 60.0), 1.0))
		if orbit and not free_camera:
			camera.position = Vector3(sin(time * 0.15) * 9.0, 2.3 + sin(time * 0.09), cos(time * 0.15) * 2.0)
			camera.look_at(Vector3(cos(time * 0.15) * 7.0, 2.5, 0))
		for i in emitters.size():
			emitters[i].position.z = -1.4 + sin(time * 0.7 + i) * 1.0
			emitters[i].material_override.emission = Color.from_hsv(fmod(i / 3.0 + time * 0.03, 1.0), 0.85, 1.0)
		for i in lights.size():
			lights[i].light_energy = 3.0 + sin(time + i) * 1.5
	if free_camera:
		var v := Vector3(float(Input.is_physical_key_pressed(KEY_D)) - float(Input.is_physical_key_pressed(KEY_A)), float(Input.is_physical_key_pressed(KEY_E)) - float(Input.is_physical_key_pressed(KEY_Q)), float(Input.is_physical_key_pressed(KEY_S)) - float(Input.is_physical_key_pressed(KEY_W)))
		camera.position += camera.basis * v * _delta * 6
	ticks += 1
	if ticks % 30 == 0 and not tod_video:
		var s := gi.get_statistics()
		var hour := fposmod(tod_phase * 24.0 + 6.0, 24.0)
		var algorithm := "Surfel GI / multi bounce" if s.get("surfel_multibounce", false) else "Surfel GI / two bounce"
		if not gi.is_enabled(): algorithm = "GI OFF"
		hud.text = "SPONZA | %02d:%02d | %s | %d FPS\nG GI   B multi bounce   Space pause   RMB + WASD fly\n%s | Cache %d slots | %s" % [int(hour), int(fmod(hour, 1.0) * 60), algorithm, Engine.get_frames_per_second(), DEBUG_VIEWS[debug_mode][0], s.get("surfel_capacity", 0), "SUN + SKY + LOCAL" if local_mode else "SUN + SKY ONLY"]
	if not specular_suite and not suite and not surfel_suite and not surfel_debug_suite and not tod_suite and not tod_video and duration > 0 and ticks == duration:
		if benchmark and output != "":
			FileAccess.open(output.path_join("benchmark.json"), FileAccess.WRITE).store_string(JSON.stringify({"renderer": RenderingServer.get_current_rendering_method(), "width": get_window().size.x, "height": get_window().size.y, "gi": gi.is_enabled(), "taa": get_viewport().use_taa, "animated": animate, "warmup_frames": 60, "frame_ms": frame_ms, "statistics": gi.get_statistics()}))
		elif output != "":
			await shot("final")
		get_tree().quit()

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_RIGHT:
		free_camera = event.pressed
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED if free_camera else Input.MOUSE_MODE_VISIBLE
	if event is InputEventMouseMotion and free_camera:
		camera.rotate_y(-event.relative.x * 0.003)
		camera.rotate_object_local(Vector3.RIGHT, -event.relative.y * 0.003)
	if event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_G: gi.set_enabled(not gi.is_enabled())
			KEY_B:
				ProjectSettings.set_setting("rendering/kiln/surfel_multibounce", not ProjectSettings.get_setting("rendering/kiln/surfel_multibounce", true))
			KEY_F2: _set_debug_view(13)
			KEY_F3: _set_debug_view(14)
			KEY_F4: _cycle_surfel_debug(1)
			KEY_F5: _cycle_surfel_debug(-1)
			KEY_F6: gi.reset_history()
			KEY_F7: _set_debug_view(0)
			KEY_F8: debug_panel.visible = not debug_panel.visible
			KEY_SPACE: animate = not animate
			KEY_C: orbit = not orbit
			KEY_F1:
				_set_debug_view(7 if debug_mode != 7 else 0)
			KEY_ESCAPE:
				free_camera = false
				Input.mouse_mode = Input.MOUSE_MODE_VISIBLE

func settle(count := 96) -> void:
	for i in count:
		await get_tree().process_frame
		await RenderingServer.frame_post_draw

func shot(label: String) -> void:
	var directory := output.path_join(label)
	gi.request_capture(directory)
	await settle(2)
	get_viewport().get_texture().get_image().save_png(directory.path_join("color.png"))
	FileAccess.open(directory.path_join("world.json"), FileAccess.WRITE).store_string(JSON.stringify(gi.get_statistics()))
	FileAccess.open(directory.path_join("lighting.json"), FileAccess.WRITE).store_string(JSON.stringify({"hour": fposmod(tod_phase * 24.0 + 6.0, 24.0), "sun_energy": sun.light_energy, "sky_energy": sky_energy_value, "local_mode": local_mode, "gi_enabled": gi.is_enabled(), "debug": debug_mode, "floor_roughness": floor_material.roughness}))
	print("[SPONZA_CAPTURE] ", label)

func _run_suite() -> void:
	assert(output != "", "--suite requires --output=<temporary directory>")
	_set_local_mode(true)
	await settle(8)
	var validation := gi.validate_bvh(256)
	FileAccess.open(output.path_join("bvh.json"), FileAccess.WRITE).store_string(JSON.stringify(validation))
	for light in lights: light.visible = false
	for emitter in emitters: emitter.material_override.emission_energy_multiplier = 0.0
	sun.light_energy = 0.0
	gi.set_lighting(Vector3.UP, Vector3.ONE, 0, 0, 0)
	await settle(128)
	await shot("00_dark")
	_set_tod(0.22)
	await settle(256)
	await shot("01_day")
	for i in 8:
		await settle(1)
		await shot("noise_%02d" % i)
	_set_tod(0.46)
	await settle(96)
	await shot("02_sunset")
	sun.light_energy = 0.0
	gi.set_lighting(Vector3.UP, Vector3.ONE, 0, 0, 0)
	for light in lights: light.visible = true
	await settle(128)
	await shot("03_local")
	for light in lights: light.visible = false
	emitters[0].material_override.emission_energy_multiplier = 6.0
	emitters[0].material_override.emission = Color(1, 0.02, 0.01)
	await settle(192)
	await shot("04_emission_red")
	var before := gi.get_statistics()
	emitters[0].material_override.emission = Color(0.01, 0.02, 1)
	await settle(96)
	var after := gi.get_statistics()
	assert(after.material_version > before.material_version, "Material change must update transport automatically")
	assert(after.geometry_version == before.geometry_version and after.dynamic_version == before.dynamic_version, "Material changes must not rebuild the BVH")
	await shot("05_emission_blue")
	emitters[0].material_override.emission_energy_multiplier = 0.0
	await settle(32)
	await shot("06_off_32")
	_set_tod(0.22)
	for light in lights: light.visible = true
	for emitter in emitters: emitter.material_override.emission_energy_multiplier = 4.0
	for i in 180:
		_set_tod(fmod(0.22 + i / 240.0, 1.0))
		camera.position = Vector3(-8 + 16 * i / 180.0, 2.2, 0.0)
		camera.look_at(camera.position + Vector3(cos(i * 0.03), 0.1, sin(i * 0.03)))
		emitters[0].position.z = sin(i * 0.05) * 2
		await settle(1)
		if i % 30 == 0: await shot("motion_%03d" % i)
	await shot("motion_end")
	await settle(32)
	await shot("motion_settled_32")
	await settle(128)
	await shot("07_settled")
	print("[SPONZA_SUITE] completed")
	get_tree().quit()

func _run_surfel_suite() -> void:
	assert(output != "")
	_set_local_mode(false)
	_set_tod(0.25)
	for mode in ["multibounce", "two_bounce", "restored"]:
		ProjectSettings.set_setting("rendering/kiln/surfel_multibounce", mode != "two_bounce")
		gi.reset_history()
		await settle(256)
		await shot(mode)
	for mode in [13, 14, 7, 0]:
		ProjectSettings.set_setting("rendering/kiln/debug_view", mode)
		await settle(8)
		await shot("debug_%d" % mode)
	var original_size := get_window().size
	get_window().size = Vector2i(961, 541)
	await settle(64)
	await shot("odd_resize")
	get_window().size = original_size
	await settle(128)
	await shot("resize_restored")
	print("[SPONZA_SURFEL_SUITE] completed")
	get_tree().quit()

func _run_tod_suite() -> void:
	assert(output != "")
	_set_local_mode(false)
	# Identical camera for every TOD / GI toggle: no local light or emission can
	# masquerade as sunlight/sky bounced into the shaded Sponza corridors.
	camera.position = Vector3(-10, 2.2, 0)
	camera.look_at(Vector3(5, 3, 0))
	for hour in [6.5, 9.0, 12.0, 17.5, 21.0]:
		_set_tod(fposmod((hour - 6.0) / 24.0, 1.0))
		gi.set_enabled(true)
		await settle(192)
		var label := "tod_%04d" % int(hour * 100)
		await shot(label + "_on")
		if hour == 12.0:
			for sample in 6:
				await settle(1)
				await shot("tod_noise_%02d" % sample)
		ProjectSettings.set_setting("rendering/kiln/debug_view", 7)
		await settle(4)
		await shot(label + "_indirect")
		ProjectSettings.set_setting("rendering/kiln/debug_view", 0)
		gi.set_enabled(false)
		await settle(32)
		await shot(label + "_off")
	gi.set_enabled(true)
	# Continuous TOD plus rotating camera, then compare bounded settling.
	for i in 240:
		_set_tod(fposmod(0.04 + i / 240.0, 1.0))
		camera.position = Vector3(-8 + 16.0 * i / 240.0, 2.2, 0.0)
		camera.look_at(camera.position + Vector3(cos(i * 0.026), 0.08, sin(i * 0.026)))
		await settle(1)
		if i % 60 == 0: await shot("tod_motion_%03d" % i)
	await settle(32)
	await shot("tod_settled_32")
	await settle(160)
	await shot("tod_reference")
	print("[SPONZA_TOD] completed; sun/sky only")
	get_tree().quit()

func _run_tod_video() -> void:
	assert(output != "")
	_set_local_mode(false)
	camera.position = Vector3(-10, 2.2, 0)
	camera.look_at(Vector3(5, 3, 0))
	_set_tod(0.02)
	await settle(128)
	controls.show()
	for child in controls.get_children(): child.visible = child == hud
	var frames := output.path_join("frames")
	DirAccess.make_dir_recursive_absolute(frames)
	# Capture cost is intentionally excluded from performance measurements.
	for i in 480:
		_set_tod(fposmod(0.02 + i / 480.0, 1.0))
		var hour := fposmod(tod_phase * 24.0 + 6.0, 24.0)
		hud.text = "SPONZA | SUN + SKY ONLY | %02d:%02d | Diffuse GI ON\nContinuous 24-hour cycle | No local lights or emissive props" % [int(hour), int(fmod(hour, 1.0) * 60)]
		await settle(1)
		get_viewport().get_texture().get_image().save_png(frames.path_join("%04d.png" % i))
	print("[SPONZA_TOD_VIDEO] 480 frames, continuous sun/sky only")
	get_tree().quit()

func _run_surfel_debug_suite() -> void:
	assert(output != "")
	_set_debug_view(15)
	_set_local_mode(true)
	_set_tod(0.25)
	await settle(160)
	for mode in range(15, 28):
		_set_debug_view(mode)
		await settle(12)
		await shot("surfel_%02d" % mode)
	_set_debug_view(19)
	gi.reset_history()
	await settle(2)
	await shot("cold_age")
	_set_debug_view(22)
	var original_size := get_window().size
	get_window().size = Vector2i(961, 541)
	await settle(64)
	await shot("debug_odd_resize")
	get_window().size = original_size
	_set_debug_view(15)
	gi.set_enabled(false)
	await settle(2)
	await shot("debug_gi_off")
	gi.set_enabled(true)
	_set_debug_view(0)
	await settle(96)
	await shot("debug_return_lit")
	FileAccess.open(output.path_join("debug_views.json"), FileAccess.WRITE).store_string(JSON.stringify(DEBUG_VIEWS, "\t"))
	print("[SPONZA_SURFEL_DEBUG] completed")
	get_tree().quit()

func _run_specular_suite() -> void:
	assert(output != "")
	camera.position = Vector3(-9, 1.6, 0.4)
	camera.look_at(Vector3(4, 1.0, 0))
	_set_local_mode(false)
	_set_tod(0.25)
	floor_material.roughness = 0.85
	await settle(160)
	await shot("dry_day")
	floor_material.roughness = 0.18
	ProjectSettings.set_setting("rendering/kiln/surfel_specular", false)
	await settle(64)
	await shot("wet_specular_off")
	ProjectSettings.set_setting("rendering/kiln/surfel_specular", true)
	await settle(96)
	await shot("wet_day")
	for i in 8:
		await settle(1)
		await shot("wet_noise_%02d" % i)
	_set_debug_view(28)
	await settle(8)
	await shot("wet_specular_only")
	floor_material.metallic_specular = 0.0
	await settle(48)
	await shot("zero_f0")
	floor_material.metallic_specular = 0.5
	floor_material.metallic = 1.0
	await settle(48)
	await shot("metallic_reflection")
	floor_material.metallic = 0.0
	_set_debug_view(3)
	await settle(2)
	await shot("wet_roughness")
	_set_debug_view(0)
	sun.light_energy = 0.0
	gi.set_lighting(Vector3.UP, Vector3.ONE, 0, 0, 0)
	for light in lights: light.visible = false
	for emitter in emitters: emitter.visible = false
	var source := emitters[0]
	source.visible = true
	source.position = Vector3(-1, 2, 0)
	source.material_override.emission = Color(1, 0.01, 0.005)
	source.material_override.emission_energy_multiplier = 8.0
	for r in [0.06, 0.18, 0.5, 0.85]:
		floor_material.roughness = r
		await settle(96)
		await shot("emitter_rough_%02d" % int(r * 100))
	floor_material.roughness = 0.18
	source.material_override.emission = Color(0.005, 0.01, 1)
	await settle(64)
	await shot("emitter_blue")
	# The source moves above the camera's field of view; the floor still sees it.
	source.position = Vector3(-1, 5.5, 0)
	camera.look_at(Vector3(4, -3.0, 0))
	await settle(64)
	await shot("offscreen_emitter")
	for i in 60:
		camera.position.z = 0.4 + sin(i * 0.04) * 0.5
		camera.look_at(Vector3(4, -3.0, 0))
		source.position.x = -1 + sin(i * 0.07)
		await settle(1)
		if i % 20 == 0: await shot("reflection_motion_%02d" % i)
	await settle(32)
	await shot("reflection_settled_32")
	await settle(96)
	await shot("reflection_reference")
	source.material_override.emission_energy_multiplier = 0.0
	await settle(32)
	await shot("reflection_off_32")
	_set_tod(0.25)
	get_window().size = Vector2i(961, 541)
	await settle(96)
	await shot("reflection_odd_resize")
	gi.set_enabled(false)
	await settle(2)
	await shot("reflection_gi_off")
	print("[SPONZA_SPECULAR] completed")
	get_tree().quit()
