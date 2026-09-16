extends SceneTree

func _initialize() -> void:
	var temporary := OS.get_environment("TEMP")
	if temporary.is_empty(): temporary = OS.get_environment("TMPDIR")
	if temporary.is_empty(): temporary = "/tmp"
	var output := temporary.path_join("kiln-import-proxy-check")
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--output="): output = arg.trim_prefix("--output=")
	DirAccess.make_dir_recursive_absolute(output)
	var sponza: Mesh = load("res://assets/sponza.obj")
	assert(sponza.has_meta("kiln_gi_proxy"), "OBJ must contain its proxy BEFORE a GI world exists")
	var sponza_proxy: KilnGIProxy = sponza.get_meta("kiln_gi_proxy")
	assert(sponza_proxy.proxy_mesh.get_surface_count() == sponza.get_surface_count())
	var merged: ArrayMesh = load("res://tests/proxy_merged.gltf")
	assert(merged.has_meta("kiln_gi_proxy"), "Single-mesh import must regenerate a proxy AFTER merging nodes")
	var merged_proxy: KilnGIProxy = merged.get_meta("kiln_gi_proxy")
	assert(merged_proxy.proxy_mesh.get_aabb().size.x > 5.9)
	assert(merged_proxy.data.surfaces[0].source_triangles == 2048)
	var scene: PackedScene = load("res://tests/proxy_cylinder.gltf")
	var model := scene.instantiate()
	var instance: MeshInstance3D = model.find_child("*", true, false) as MeshInstance3D
	assert(instance != null)
	var mesh := instance.mesh
	assert(mesh.has_meta("kiln_gi_proxy"), "glTF must serialize its proxy during scene import")
	var proxy: KilnGIProxy = mesh.get_meta("kiln_gi_proxy")
	var triangles := proxy.proxy_mesh.surface_get_array_index_len(0) / 3
	assert(triangles < mesh.surface_get_array_index_len(0) / 3)
	var material: StandardMaterial3D = mesh.surface_get_material(0)
	var revision := proxy.get_material_revision()
	material.albedo_color = Color(0.05, 0.8, 0.1)
	assert(proxy.get_material_revision() > revision)
	var solid: StandardMaterial3D = proxy.proxy_mesh.surface_get_material(0)
	assert(solid.albedo_color.is_equal_approx(material.albedo_color), "Color edits update solid proxy without GI world/reimport")
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.8, 0.1, 0.1))
	var texture := ImageTexture.create_from_image(image)
	material.albedo_color = Color.WHITE
	material.albedo_texture = texture
	assert(solid.albedo_color.r > solid.albedo_color.b * 3)
	image.fill(Color(0.05, 0.1, 0.9))
	texture.update(image)
	assert(solid.albedo_color.b > solid.albedo_color.r * 3, "In-place texture update refreshes proxy")
	material.emission_enabled = true
	material.emission = Color(0.2, 0.1, 0.8)
	material.emission_energy_multiplier = 3.0
	assert(solid.emission_enabled and solid.emission.b > solid.emission.r)
	var stored := output.path_join("roundtrip.res")
	assert(ResourceSaver.save(mesh, stored) == OK)
	var reloaded: Mesh = ResourceLoader.load(stored, "", ResourceLoader.CACHE_MODE_IGNORE_DEEP)
	var reloaded_proxy: KilnGIProxy = reloaded.get_meta("kiln_gi_proxy")
	var reloaded_material: StandardMaterial3D = reloaded.surface_get_material(0)
	reloaded_material.albedo_texture = null
	reloaded_material.albedo_color = Color(0.9, 0.1, 0.05)
	assert(reloaded_proxy.proxy_mesh.surface_get_material(0).albedo_color.is_equal_approx(reloaded_material.albedo_color), "Loaded proxy reconnects source changes")
	var replacement := StandardMaterial3D.new()
	replacement.albedo_color = Color(0.25, 0.65, 0.9)
	reloaded.surface_set_material(0, replacement)
	assert(reloaded_proxy.proxy_mesh.surface_get_material(0).albedo_color.is_equal_approx(replacement.albedo_color), "Replacing an imported surface material refreshes the proxy")
	replacement.albedo_color = Color(0.8, 0.3, 0.2)
	assert(reloaded_proxy.proxy_mesh.surface_get_material(0).albedo_color.is_equal_approx(replacement.albedo_color), "Replacement material remains observed")
	assert(proxy.proxy_mesh.surface_get_array_index_len(0) / 3 == triangles, "Material-only edits reuse proxy geometry")
	FileAccess.open(output.path_join("checks.json"), FileAccess.WRITE).store_string(JSON.stringify({"passed": true, "obj_surfaces": sponza_proxy.proxy_mesh.get_surface_count(), "gltf_proxy_triangles": triangles, "source_triangles": mesh.surface_get_array_index_len(0) / 3, "without_gi_world": true, "material_revision": proxy.get_material_revision()}, "\t"))
	model.free()
	print("[IMPORT_PROXY] passed: OBJ, glTF, tint, texture, emission and serialization without GI world")
	quit()
