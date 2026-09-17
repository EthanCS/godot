extends SceneTree

func _initialize() -> void:
    var output := OS.get_environment("TEMP").path_join("kiln-import-geometry-check")
    for arg in OS.get_cmdline_user_args():
        if arg.begins_with("--output="): output = arg.trim_prefix("--output=")
    DirAccess.make_dir_recursive_absolute(output)
    var sponza: Mesh = load("res://assets/sponza.obj")
    assert(not sponza.has_meta("kiln_gi_proxy"), "OBJ import must not create a second GI mesh")
    var merged: ArrayMesh = load("res://tests/proxy_merged.gltf")
    assert(not merged.has_meta("kiln_gi_proxy"))
    assert(merged.get_aabb().size.x > 5.9)
    var scene: PackedScene = load("res://tests/proxy_cylinder.gltf")
    var model := scene.instantiate()
    var instance := model.find_child("*", true, false) as MeshInstance3D
    assert(instance != null)
    var mesh := instance.mesh as ArrayMesh
    assert(not mesh.has_meta("kiln_gi_proxy"), "glTF import must only serialize the original mesh")
    var count: int = mesh.surface_get_array_index_len(0)
    var stored := output.path_join("roundtrip.res")
    assert(ResourceSaver.save(mesh, stored) == OK)
    var reloaded := ResourceLoader.load(stored, "", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as Mesh
    assert(not reloaded.has_meta("kiln_gi_proxy"))
    assert(reloaded.surface_get_array_index_len(0) == count)
    FileAccess.open(output.path_join("checks.json"), FileAccess.WRITE).store_string(JSON.stringify({"passed": true, "obj_surfaces": sponza.get_surface_count(), "original_mesh_roundtrip": true, "generated_proxy": false}, "\t"))
    model.free()
    print("[IMPORT_GEOMETRY] passed: OBJ/glTF/merged source meshes and serialization without proxies")
    quit()
