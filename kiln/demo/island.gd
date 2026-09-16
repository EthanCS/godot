extends Node3D

var config: Dictionary = JSON.parse_string(FileAccess.get_file_as_string("res://timeline.json"))
var gi: KilnGIWorld
var gi_enabled := false
var ao_enabled := true
var authored_emissions: Dictionary = {}
var camera: Camera3D
var sun: DirectionalLight3D
var environment: Environment
var sky_material: ShaderMaterial
var hud: Label
var diagnostics: PanelContainer
var diagnostics_text: Label
var show_diagnostics := false
var lights: Array[Light3D] = []
var emitters: Array[MeshInstance3D] = []
var occluders: Array[MeshInstance3D] = []
var elapsed := 0.0
var camera_time := 0.0
var light_time := 0.0
var emission_time := 0.0
var tod_time := 0.0
var camera_motion := true
var light_motion := true
var emission_motion := true
var tod_motion := true
var paused := false
var dense := false
var preset := 3
var shadows := 8
var fixed_view := -1
var frames := 0
var capture_dir := ""
var capture_at := 3.0
var stop_after := 0.0
var replay_time := -1.0
var material_library: Dictionary = {}
var frame_times: Array[float] = []
var last_usec := 0
var reference := false
var hide_hud := false
var report_dir := ""
var buffer_at := -1.0
var capture_interval := 1.0
var free_camera := false
var pending_size := Vector2i.ZERO
var warmup := 3.0
var sample_times: Array[float] = []
var profile_samples: Array = []
var runtime_statistics: Dictionary = {}
var profiling := false
var benchmark := false
var benchmark_dynamic := false
var measurement_started := -1.0
var last_profile := -1
var lifecycle := false
var lifecycle_step := 0
var tod_speed := 1.0

func _ready() -> void:
	shadows = int(config.shadow_lights)
	for arg in OS.get_cmdline_user_args():
		if arg == "--profile": profiling = true
		if arg == "--benchmark": benchmark = true
		if arg == "--benchmark-dynamic":
			benchmark = true
			benchmark_dynamic = true
		if arg == "--lifecycle": lifecycle = true
		if arg == "--reference": reference = true
		if arg == "--no-hud": hide_hud = true
		if arg == "--diagnostics": show_diagnostics = true
		if arg.begins_with("--report-dir="): report_dir = arg.trim_prefix("--report-dir=")
		if arg.begins_with("--buffer-at="): buffer_at = float(arg.get_slice("=", 1))
		if arg.begins_with("--capture-at="): capture_at = float(arg.get_slice("=", 1))
		if arg.begins_with("--capture-interval="): capture_interval = float(arg.get_slice("=", 1))
		if arg.begins_with("--warmup="): warmup = float(arg.get_slice("=", 1))
		if arg.begins_with("--size="):
			var xy := arg.get_slice("=", 1).split("x")
			pending_size = Vector2i(int(xy[0]), int(xy[1]))
		if arg.begins_with("--lights="): config.lights = int(arg.get_slice("=", 1))
		if arg.begins_with("--shadows="): shadows = int(arg.get_slice("=", 1))
		if arg.begins_with("--capture-dir="): capture_dir = arg.trim_prefix("--capture-dir=")
		if arg.begins_with("--duration="): stop_after = float(arg.get_slice("=", 1))
		if arg.begins_with("--time="): replay_time = float(arg.get_slice("=", 1))
		if arg.begins_with("--view="): fixed_view = int(arg.get_slice("=", 1))
		if arg.begins_with("--preset="): preset = int(arg.get_slice("=", 1))
		if arg == "--no-ao": ao_enabled = false
		if arg == "--gi": gi_enabled = true
		if arg == "--no-gi": gi_enabled = false
		if arg == "--no-aa": get_viewport().use_taa = false
		if arg == "--dense": dense = true
		if arg.begins_with("--tod-speed="): tod_speed = float(arg.get_slice("=", 1))
		if arg.begins_with("--light-range="): config.light_range = maxf(0.5, float(arg.get_slice("=", 1)))
		if arg.begins_with("--debug="): ProjectSettings.set_setting("rendering/kiln/debug_view", int(arg.get_slice("=", 1)))
	if reference:
		config.lights = 0
		config.emitters = 0
		shadows = 0
		replay_time = 0.0
		if fixed_view < 0: fixed_view = 2
	if pending_size != Vector2i.ZERO: get_window().size = pending_size
	if capture_dir != "": DirAccess.make_dir_recursive_absolute(capture_dir)
	var manifest: Dictionary = JSON.parse_string(FileAccess.get_file_as_string("res://assets/models/environments/main_island/material_manifest.json"))
	for key in manifest:
		material_library[key] = load(manifest[key].resource)
		authored_emissions[key] = material_library[key].get_shader_parameter("authored_emission")
	_apply_preset()
	var island: Node3D = load("res://assets/models/environments/main_island/main_island.glb").instantiate()
	add_child(island)
	var count := _bind_island(island)
	assert(count == 797, "Island surface contract changed: %d" % count)
	print("[KILN_DEMO] surfaces=%d materials=%d renderer=%s GI=native" % [count, material_library.size(), RenderingServer.get_current_rendering_method()])
	camera = Camera3D.new()
	camera.fov = 44.0
	camera.near = 0.2
	camera.far = 500.0
	add_child(camera)
	camera.make_current()
	sun = DirectionalLight3D.new()
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 260.0
	sun.directional_shadow_blend_splits = true
	sun.light_angular_distance = 1.0
	sun.shadow_bias = 0.1
	sun.shadow_normal_bias = 0.7
	add_child(sun)
	environment = Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	environment.tonemap_exposure = 1.0
	environment.glow_enabled = false
	environment.sky = Sky.new()
	sky_material = ShaderMaterial.new()
	sky_material.shader = load("res://rendering/sky/sky.gdshader")
	environment.sky.sky_material = sky_material
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)
	_build_lights(int(config.lights))
	for i in int(config.emitters):
		var mesh := MeshInstance3D.new()
		var box := BoxMesh.new()
		box.size = Vector3(1.3, 2.4, 1.3)
		mesh.mesh = box
		mesh.set_meta("kiln_dynamic", true)
		var mat := ShaderMaterial.new()
		mat.shader = load("res://rendering/shaders/main_island_surface.gdshader")
		mat.set_shader_parameter("tint_linear", Vector3(0.25, 0.25, 0.25))
		mesh.material_override = mat
		add_child(mesh)
		emitters.append(mesh)
	for i in (0 if reference else 4):
		var mesh := MeshInstance3D.new()
		var box := BoxMesh.new()
		box.size = Vector3(2.0, 4.0, 0.8)
		mesh.mesh = box
		mesh.set_meta("kiln_dynamic", true)
		mesh.material_override = material_library.plaster
		add_child(mesh)
		occluders.append(mesh)
	gi = KilnGIWorld.new()
	gi.environment = environment
	gi.set_quality(int(config.gi_rays_per_frame), int(config.gi_convergence_samples))
	gi.enabled = gi_enabled
	gi.set_ao_enabled(ao_enabled)
	add_child(gi)
	gi.set_profiling(profiling)
	if reference:
		gi.set_sky(Vector3(0.534, 0.478, 0.771), Vector3(0.0296, 0.0606, 0.1018), false)
		environment.background_mode = Environment.BG_COLOR
		environment.background_color = Color(0.19, 0.225, 0.235)
	var canvas := CanvasLayer.new()
	add_child(canvas)
	hud = Label.new()
	hud.position = Vector2(18, 16)
	hud.add_theme_color_override("font_shadow_color", Color.BLACK)
	hud.add_theme_constant_override("shadow_offset_x", 2)
	hud.add_theme_constant_override("shadow_offset_y", 2)
	canvas.add_child(hud)
	hud.visible = not hide_hud
	_build_diagnostics(canvas)
	last_usec = Time.get_ticks_usec()

func _build_diagnostics(canvas: CanvasLayer) -> void:
	diagnostics = PanelContainer.new()
	diagnostics.position = Vector2(18, 150)
	diagnostics.visible = show_diagnostics and not hide_hud
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.035, 0.045, 0.055, 0.96)
	style.content_margin_left = 12
	style.content_margin_right = 12
	style.content_margin_top = 12
	style.content_margin_bottom = 12
	diagnostics.add_theme_stylebox_override("panel", style)
	canvas.add_child(diagnostics)
	var rows := VBoxContainer.new()
	diagnostics.add_child(rows)
	var title := Label.new()
	title.text = "Kiln diagnostics · F1 to collapse"
	rows.add_child(title)
	var view := OptionButton.new()
	view.disabled = RenderingServer.get_current_rendering_method() != "kiln_deferred"
	view.tooltip_text = "Channel views are provided by the Kiln deferred resolve pass."
	for name in ["Lit", "Albedo / metallic", "Normal", "Roughness", "Authored emission", "Material channels", "Depth", "Indirect light", "AO", "Motion vectors", "Direct light", "Cluster occupancy", "History confidence"]:
		view.add_item(name)
	view.select(clampi(int(ProjectSettings.get_setting("rendering/kiln/debug_view", 0)), 0, 12))
	view.item_selected.connect(func(index: int): ProjectSettings.set_setting("rendering/kiln/debug_view", index))
	rows.add_child(view)
	diagnostics_text = Label.new()
	diagnostics_text.custom_minimum_size.x = 410
	diagnostics_text.add_theme_font_size_override("font_size", 14)
	rows.add_child(diagnostics_text)
	var reset := Button.new()
	reset.text = "Reset GI history"
	reset.pressed.connect(func(): gi.reset_history())
	rows.add_child(reset)
	var help := Label.new()
	help.add_theme_font_size_override("font_size", 14)
	help.text = "G / O / A: GI / AO / TAA   ·   B: bloom\n, / .: slow / fast TOD   ·   − / =: light range\n[ / ]: shift TOD   ·   S: shadow budget"
	rows.add_child(help)

func _update_diagnostics() -> void:
	var s := gi.get_statistics()
	diagnostics_text.text = ("Uploaded lights: %d omni + %d spot\nLocal shadows: %d active / %d configured\nCluster capacity: %d   overflow: %d\nTriangles: %d static + %d dynamic\nVersions · geometry %d / rigid %d / material %d\nVersions · lighting %d / history %d\nGI: %s   AO: %s   TAA: %s\nGI budget: %d rays (≥4 when lighting changes), %d samples\nLight range: %.1f m   TOD speed: %.2f×\nVideo memory: %.1f MiB   GPU timers: N/A" % [s.get("uploaded_omni", 0), s.get("uploaded_spot", 0), s.get("uploaded_local_shadows", 0), shadows, s.get("cluster_capacity", 0), s.get("light_overflow", 0), s.get("static_triangles", 0), s.get("dynamic_triangles", 0), s.get("geometry_version", 0), s.get("dynamic_version", 0), s.get("material_version", 0), s.get("light_version", 0), s.get("history_version", 0), "on" if gi_enabled else "off", "on" if ao_enabled else "off", "on" if get_viewport().use_taa else "off", s.get("rays_per_frame", 1), s.get("convergence_samples", 256), float(config.light_range), tod_speed, Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED) / 1048576.0])

func _bind_island(node: Node) -> int:
	var count := 0
	if node is MeshInstance3D:
		for i in node.mesh.get_surface_count():
			var source: Material = node.mesh.surface_get_material(i)
			assert(source != null and material_library.has(source.resource_name))
			node.set_surface_override_material(i, material_library[source.resource_name])
			count += 1
	for child in node.get_children(): count += _bind_island(child)
	if str(node.name).begins_with("Boat"): node.visible = false
	return count

func _build_lights(count: int) -> void:
	for light in lights: light.queue_free()
	lights.clear()
	for i in count:
		var light: Light3D = SpotLight3D.new() if i % 4 == 3 else OmniLight3D.new()
		light.light_energy = float(config.light_energy)
		light.light_color = Color.from_hsv(fmod(i * 0.61803398875, 1.0), 0.7, 1.0)
		light.shadow_enabled = i < shadows
		if light is SpotLight3D:
			light.spot_range = float(config.light_range) * 1.5
			light.spot_angle = 48.0
		else:
			light.omni_range = float(config.light_range)
		add_child(light)
		lights.append(light)

func _process(delta: float) -> void:
	frames += 1
	var now := Time.get_ticks_usec()
	delta = float(now - last_usec) / 1000000.0
	elapsed += delta
	if (stop_after > 0.0 or report_dir != "" or benchmark) and elapsed > warmup and (not benchmark or frames >= (320 if gi_enabled else 120)):
		if measurement_started < 0.0:
			measurement_started = elapsed
			if benchmark_dynamic:
				camera_time = 0.0
				light_time = 0.0
				emission_time = 0.0
				tod_time = 0.0
		frame_times.append((now - last_usec) / 1000.0)
		sample_times.append(elapsed)
	last_usec = now
	if profiling and measurement_started >= 0.0:
		var stats := gi.get_statistics()
		if int(stats.profile_frame) != last_profile:
			last_profile = int(stats.profile_frame)
			profile_samples.append(stats)
	if lifecycle and elapsed > float(lifecycle_step + 1) * 4.0:
		lifecycle_step += 1
		get_window().size = Vector2i(1001, 703) if lifecycle_step % 2 else Vector2i(1920, 1080)
		fixed_view = lifecycle_step % 5
		gi.reset_history()
	if replay_time >= 0.0:
		camera_time = replay_time
		light_time = replay_time
		emission_time = replay_time
		tod_time = replay_time
	elif not paused and (not benchmark_dynamic or measurement_started >= 0.0):
		if camera_motion: camera_time += delta
		if light_motion: light_time += delta
		if emission_motion: emission_time += delta
		if tod_motion: tod_time += delta * tod_speed
	_update_camera()
	var day := fposmod(0.175 + tod_time / float(config.tod_period), 1.0)
	var angle := day * TAU
	sun.rotation = Vector3(-angle, deg_to_rad(35.0), 0.0)
	var daylight := smoothstep(-0.10, 0.3, sin(angle))
	sun.light_energy = daylight
	sun.light_color = Color(1.0, 0.55, 0.3).lerp(Color(1.0, 0.97, 0.91), daylight)
	sun.visible = preset >= 2
	sky_material.set_shader_parameter("time_of_day", day)
	sky_material.set_shader_parameter("energy", lerpf(0.015, 1.0, daylight) if preset >= 2 else 0.0)
	if reference:
		sun.rotation_degrees = Vector3(-45, 35, 0)
		sun.light_energy = 1.0
		sun.light_color = Color(1.0, 0.97, 0.91)
	var sun_color := sun.light_color.srgb_to_linear()
	gi.set_lighting(sun.global_basis.z, Vector3(sun_color.r, sun_color.g, sun_color.b), sun.light_energy if sun.visible else 0.0, (1.0 if reference else lerpf(0.015, 1.0, daylight)) if preset >= 2 else 0.0, day)
	for i in lights.size():
		var light := lights[i]
		var phase := i * 2.3999632297 + light_time * (0.2 + (i % 7) * 0.007)
		var radius := (3.0 + (i % 8) * 0.3) if dense else (8.0 + (i % 17) * 1.8)
		light.position = Vector3(cos(phase) * radius, 5.0 + (i % 5) * 1.7 + sin(phase * 1.3), sin(phase) * radius * 0.75)
		if light is SpotLight3D: light.look_at(light.position + Vector3(sin(phase), -2.0, cos(phase)))
		light.visible = preset == 1 or preset == 3
	for i in emitters.size():
		var mesh := emitters[i]
		var phase := i * TAU / emitters.size() + emission_time * 0.27
		mesh.position = Vector3(cos(phase) * 17.0, 5.0 + sin(phase * 1.7) * 1.5, sin(phase) * 11.0)
		mesh.rotation = Vector3(phase * 0.2, phase, phase * 0.1)
		var color := Color.from_hsv(fmod(i / 12.0 + emission_time * 0.025, 1.0), 0.85, 1.0).srgb_to_linear()
		var power := float(config.emission_energy) * (0.6 + 0.4 * sin(emission_time * 1.5 + i))
		if fmod(emission_time + i, 11.0) < 2.0: power = 0.0
		if preset == 1 or preset == 2: power = 0.0
		mesh.material_override.set_shader_parameter("authored_emission", Vector3(color.r, color.g, color.b) * power)
	for i in occluders.size():
		occluders[i].position = Vector3(-9.0 + i * 6.0, 5.5, 7.0 + sin(emission_time * 0.7 + i) * 3.0)
		occluders[i].rotation.y = emission_time * 0.25 + i
	if frames % 15 == 0:
		runtime_statistics = gi.get_statistics()
		if diagnostics.visible: _update_diagnostics()
	hud.text = "%s • %.1f ms\n%d local lights • %d active local shadows • TOD %.2f\nKiln GI: %s (software BVH)\nSpace pause | C camera | T TOD | L lights | E emission\n1–4 count | S shadows | D dense | P isolation | A TAA | F view | Tab free | Home reset | F1 diagnostics" % [RenderingServer.get_current_rendering_method(), delta * 1000.0, lights.size(), int(runtime_statistics.get("uploaded_local_shadows", 0)), day, "on" if gi_enabled else "off"]
	if capture_dir != "" and elapsed >= capture_at:
		capture_at += capture_interval
		_capture()
	if free_camera:
		var input := Vector3(float(Input.is_physical_key_pressed(KEY_D)) - float(Input.is_physical_key_pressed(KEY_A)), float(Input.is_physical_key_pressed(KEY_E)) - float(Input.is_physical_key_pressed(KEY_Q)), float(Input.is_physical_key_pressed(KEY_S)) - float(Input.is_physical_key_pressed(KEY_W)))
		camera.position += camera.basis * input * delta * (30.0 if Input.is_key_pressed(KEY_SHIFT) else 10.0)
	if buffer_at >= 0.0 and elapsed >= buffer_at:
		buffer_at = -1.0
		gi.request_capture(capture_dir.path_join("buffers"))
	if stop_after > 0.0 and ((not benchmark and elapsed >= stop_after) or (benchmark and measurement_started >= 0.0 and elapsed - measurement_started >= stop_after - warmup)):
		_save_report()
		get_tree().quit()

func _update_camera() -> void:
	if free_camera: return
	if fixed_view >= 0:
		var v: Array = config.fixed_views[fixed_view % 5]
		camera.position = Vector3(v[0][0], v[0][1], v[0][2])
		camera.look_at(Vector3(v[1][0], v[1][1], v[1][2]))
		return
	var phase := fposmod(camera_time / float(config.camera_period), 1.0) * float(config.camera_keys.size() - 1)
	var index := int(floor(phase))
	var blend := smoothstep(0.0, 1.0, phase - float(index))
	var a: Array = config.camera_keys[index]
	var b: Array = config.camera_keys[index + 1]
	camera.position = Vector3(a[0][0], a[0][1], a[0][2]).lerp(Vector3(b[0][0], b[0][1], b[0][2]), blend)
	camera.look_at(Vector3(a[1][0], a[1][1], a[1][2]).lerp(Vector3(b[1][0], b[1][1], b[1][2]), blend))

func _capture() -> void:
	await RenderingServer.frame_post_draw
	var path := capture_dir.path_join("frame_%06d.png" % frames)
	get_viewport().get_texture().get_image().save_png(path)

func _save_report() -> void:
	var sorted_times := frame_times.duplicate()
	sorted_times.sort()
	var report := {"renderer": RenderingServer.get_current_rendering_method(), "seconds": elapsed, "frames": frames, "lights": lights.size(), "shadows": shadows, "taa": get_viewport().use_taa, "size": get_viewport().size, "gpu": RenderingServer.get_video_adapter_name(), "gpu_timestamps": "N/A", "gi": "native BVH" if gi_enabled else "off", "captures_during_timing": capture_dir != "", "frame_ms": frame_times, "sample_seconds": sample_times, "ao": ao_enabled, "dense": dense, "preset": preset, "replay_time": replay_time, "warmup_seconds": warmup, "world": gi.get_statistics(), "video_memory_bytes": Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED), "engine": Engine.get_version_info(), "profiles": profile_samples}
	if frame_times.size() > 0:
		for q in [0.5, 0.95, 0.99]: report[str(q)] = sorted_times[mini(int(sorted_times.size() * q), sorted_times.size() - 1)]
	var output := report_dir if report_dir != "" else (capture_dir if capture_dir != "" else OS.get_environment("TMPDIR").path_join("kiln-demo"))
	DirAccess.make_dir_recursive_absolute(output)
	FileAccess.open(output.path_join("run.json"), FileAccess.WRITE).store_string(JSON.stringify(report, "\t"))

func _unhandled_input(event: InputEvent) -> void:
	if free_camera and event is InputEventMouseMotion and Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT):
		camera.rotate_y(-event.relative.x * 0.003)
		camera.rotate_object_local(Vector3.RIGHT, -event.relative.y * 0.003)

func _unhandled_key_input(event: InputEvent) -> void:
	if not event.is_pressed() or event.is_echo(): return
	if free_camera and event.keycode in [KEY_W, KEY_A, KEY_S, KEY_D, KEY_Q, KEY_E]: return
	match event.keycode:
		KEY_F1:
			diagnostics.visible = not diagnostics.visible
			if diagnostics.visible: _update_diagnostics()
		KEY_TAB: free_camera = not free_camera
		KEY_HOME:
			free_camera = false
			fixed_view = -1
			camera_time = 0.0
		KEY_B: environment.glow_enabled = not environment.glow_enabled
		KEY_R: get_tree().reload_current_scene()
		KEY_COMMA: tod_speed = 0.25 if tod_speed == 1.0 else 1.0
		KEY_PERIOD: tod_speed = 4.0 if tod_speed == 1.0 else 1.0
		KEY_MINUS, KEY_EQUAL:
			config.light_range = clampf(float(config.light_range) * (0.8 if event.keycode == KEY_MINUS else 1.25), 0.5, 100.0)
			for light in lights:
				if light is SpotLight3D: light.spot_range = float(config.light_range) * 1.5
				else: light.omni_range = float(config.light_range)
		KEY_BRACKETLEFT: tod_time -= 5.0
		KEY_BRACKETRIGHT: tod_time += 5.0
		KEY_SPACE: paused = not paused
		KEY_C: camera_motion = not camera_motion
		KEY_T: tod_motion = not tod_motion
		KEY_L: light_motion = not light_motion
		KEY_E: emission_motion = not emission_motion
		KEY_D: dense = not dense
		KEY_A: get_viewport().use_taa = not get_viewport().use_taa
		KEY_G:
			gi_enabled = not gi_enabled
			gi.enabled = gi_enabled
		KEY_O:
			ao_enabled = not ao_enabled
			gi.set_ao_enabled(ao_enabled)
		KEY_P:
			preset = (preset + 1) % 4
			_apply_preset()
		KEY_F: fixed_view = (fixed_view + 2) % 6 - 1
		KEY_S:
			var idx: int = config.shadow_presets.find(shadows)
			shadows = int(config.shadow_presets[(idx + 1) % 4])
			for i in lights.size(): lights[i].shadow_enabled = i < shadows
		KEY_1, KEY_2, KEY_3, KEY_4: _build_lights(int(config.light_presets[event.keycode - KEY_1]))
		KEY_ESCAPE: get_tree().quit()

func _apply_preset() -> void:
	for key in material_library:
		material_library[key].set_shader_parameter("authored_emission", authored_emissions[key] if preset == 0 or preset == 3 else Vector3.ZERO)
	if gi != null: gi.rebuild()
