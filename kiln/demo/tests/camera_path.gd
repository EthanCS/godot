extends SceneTree
# Camera swept-segment clearance against the actual island triangle collision mesh.
# This CPU geometry check complements, and does not replace, real GPU tour capture.
var bodies := 0
func _initialize() -> void:
	call_deferred("run")
func collisions(node: Node) -> void:
	if str(node.name).begins_with("Boat"): node.visible = false
	if node is MeshInstance3D and node.is_visible_in_tree():
		var body := StaticBody3D.new()
		var shape := CollisionShape3D.new()
		shape.shape = node.mesh.create_trimesh_shape()
		body.add_child(shape)
		node.add_child(body)
		bodies += 1
	for child in node.get_children(): collisions(child)
func run() -> void:
	var island: Node3D = load("res://assets/models/environments/main_island/main_island.glb").instantiate()
	root.add_child(island)
	collisions(island)
	await physics_frame
	await physics_frame
	var config: Dictionary = JSON.parse_string(FileAccess.get_file_as_string("res://timeline.json"))
	var failures := []
	var rays := 0
	for i in range(config.camera_keys.size() - 1):
		var a: Array = config.camera_keys[i][0]
		var b: Array = config.camera_keys[i + 1][0]
		for offset in [Vector3.ZERO, Vector3(0.3, 0, 0), Vector3(-0.3, 0, 0), Vector3(0, 0.3, 0), Vector3(0, -0.3, 0), Vector3(0, 0, 0.3), Vector3(0, 0, -0.3)]:
			var query := PhysicsRayQueryParameters3D.create(Vector3(a[0], a[1], a[2]) + offset, Vector3(b[0], b[1], b[2]) + offset)
			query.hit_back_faces = true
			var hit := island.get_world_3d().direct_space_state.intersect_ray(query)
			rays += 1
			if not hit.is_empty(): failures.append({"segment": i, "offset": offset, "position": hit.position, "mesh": str(hit.collider.get_parent().name)})
	var report := {"passed": failures.is_empty(), "rays": rays, "meshes": bodies, "failures": failures}
	FileAccess.open("/tmp/kiln-camera-path.json", FileAccess.WRITE).store_string(JSON.stringify(report, "\t"))
	print("[KILN_CAMERA_PATH] ", report)
	quit(0 if failures.is_empty() else 1)
