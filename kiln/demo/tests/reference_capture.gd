extends "res://island.gd"
func wait_frames(count: int) -> void:
	for i in count:
		await get_tree().process_frame
		await RenderingServer.frame_post_draw
func _ready() -> void:
	super._ready()
	hud.visible = false
	stop_after = 0
	capture_dir = ""
	var output := "/tmp/kiln-reference-fiveviews"
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--output="): output = arg.trim_prefix("--output=")
	DirAccess.make_dir_recursive_absolute(output)
	for view in 5:
		fixed_view = view
		gi_enabled = true
		gi.enabled = true
		gi.set_ao_enabled(true)
		ProjectSettings.set_setting("rendering/kiln/debug_view", 0)
		await wait_frames(280)
		get_viewport().get_texture().get_image().save_png(output.path_join("reference_%d_lit.png" % view))
		ProjectSettings.set_setting("rendering/kiln/debug_view", 7)
		await wait_frames(2)
		get_viewport().get_texture().get_image().save_png(output.path_join("reference_%d_indirect.png" % view))
		var background := environment.background_color
		environment.background_color = Color.BLACK
		ProjectSettings.set_setting("rendering/kiln/debug_view", 4)
		await wait_frames(2)
		get_viewport().get_texture().get_image().save_png(output.path_join("reference_%d_emission.png" % view))
		environment.background_color = background
		ProjectSettings.set_setting("rendering/kiln/debug_view", 1)
		await wait_frames(2)
		get_viewport().get_texture().get_image().save_png(output.path_join("reference_%d_albedo.png" % view))
		ProjectSettings.set_setting("rendering/kiln/debug_view", 0)
		gi_enabled = false
		gi.enabled = false
		gi.set_ao_enabled(false)
		await wait_frames(12)
		get_viewport().get_texture().get_image().save_png(output.path_join("reference_%d_direct.png" % view))
		print("[KILN_REFERENCE] native view ", view)
	get_tree().quit()
