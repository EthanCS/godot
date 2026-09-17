extends SceneTree
## Real-device regression for RD -> Metal acceleration structures and ray queries.
## Run with --rendering-driver metal --script res://tests/metal_ray_query.gd.
var rd: RenderingDevice
var resources: Array[RID] = []
var checks := 0
var candidates := false

func _initialize() -> void:
	call_deferred("run")

func keep(rid: RID) -> RID:
	assert(rid.is_valid())
	resources.append(rid)
	return rid

func geometry(z: float, indexed: bool) -> RDAccelerationStructureGeometry:
	# Nonzero byte offsets and padded vertex strides exercise the driver layout.
	var vertices := PackedFloat32Array([0, 0, 0, 0])
	for x in [0.0, 3.0]:
		for v in [Vector3(-1, -1, z), Vector3(1, -1, z), Vector3(0, 1, z)]:
			vertices.append_array(PackedFloat32Array([v.x + x, v.y, v.z, 0]))
	var bytes := vertices.to_byte_array()
	var flags := RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT | RenderingDevice.BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT
	var g := RDAccelerationStructureGeometry.new()
	g.vertex_buffer = keep(rd.vertex_buffer_create(bytes.size(), bytes, flags))
	g.vertex_offset = 16
	g.vertex_stride = 16
	g.vertex_count = 6
	g.vertex_format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	g.flags = RenderingDevice.ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT
	if indexed:
		var indices := PackedByteArray()
		indices.resize(18)
		for i in 6: indices.encode_u16(6 + i * 2, i)
		g.index_buffer = keep(rd.index_buffer_create(9, RenderingDevice.INDEX_BUFFER_FORMAT_UINT16, indices, false, flags))
		g.index_offset = 6
		g.index_count = 6
	return g

func instance(blas: RID, id: int, mask: int, transform: Transform3D) -> RDAccelerationStructureInstance:
	var i := RDAccelerationStructureInstance.new()
	i.blas = blas
	i.id = id
	i.mask = mask
	i.transform = transform
	i.flags = RenderingDevice.ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT
	return i

func run() -> void:
	candidates = "--query-candidates" in OS.get_cmdline_user_args()
	rd = RenderingServer.create_local_rendering_device()
	assert(rd != null and rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY))
	assert(not rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE))
	var source := RDShaderSource.new()
	source.source_compute = """#version 460
#extension GL_EXT_ray_query : require
layout(local_size_x=1) in;
layout(set=0,binding=0) uniform accelerationStructureEXT scene;
layout(set=0,binding=1,std430) readonly buffer Rays { vec4 rays[]; };
struct Result { uvec4 ids; vec4 detail; };
layout(set=0,binding=2,std430) writeonly buffer Results { Result result[]; };
void main() {
    uint i=gl_GlobalInvocationID.x;
    rayQueryEXT q;
#ifdef TEST_CANDIDATES
    rayQueryInitializeEXT(q,scene,gl_RayFlagsNoOpaqueEXT,uint(rays[i*2].w),rays[i*2].xyz,0.001,rays[i*2+1].xyz,100.0);
#else
    rayQueryInitializeEXT(q,scene,gl_RayFlagsOpaqueEXT,uint(rays[i*2].w),rays[i*2].xyz,0.001,rays[i*2+1].xyz,100.0);
#endif
    bool initially_empty = rayQueryGetIntersectionTypeEXT(q,true)==gl_RayQueryCommittedIntersectionNoneEXT;
    while(rayQueryProceedEXT(q)) {
#ifdef TEST_CANDIDATES
        if(rayQueryGetIntersectionTypeEXT(q,false)==gl_RayQueryCandidateIntersectionTriangleEXT)
            rayQueryConfirmIntersectionEXT(q);
#endif
    }
    result[i].ids=uvec4(0xffffffffu);
    result[i].detail=vec4(0);
    if(rayQueryGetIntersectionTypeEXT(q,true)!=gl_RayQueryCommittedIntersectionNoneEXT) {
        result[i].ids=uvec4(rayQueryGetIntersectionInstanceCustomIndexEXT(q,true),rayQueryGetIntersectionInstanceIdEXT(q,true),rayQueryGetIntersectionGeometryIndexEXT(q,true),rayQueryGetIntersectionPrimitiveIndexEXT(q,true));
        result[i].detail=vec4(rayQueryGetIntersectionTEXT(q,true),rayQueryGetIntersectionBarycentricsEXT(q,true),0);
    }
    result[i].detail.w=initially_empty?0.0:1.0;
}
"""
	if candidates:
		source.source_compute = source.source_compute.replace("#version 460", "#version 460\n#define TEST_CANDIDATES")
	var spirv := rd.shader_compile_spirv_from_source(source)
	assert(spirv.get_stage_compile_error(RenderingDevice.SHADER_STAGE_COMPUTE).is_empty())
	var shader := keep(rd.shader_create_from_spirv(spirv))
	var pipeline := keep(rd.compute_pipeline_create(shader))
	var blas := keep(rd.blas_create([geometry(0, true), geometry(-2, false)], RenderingDevice.ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT))
	assert(rd.blas_build(blas) == OK)
	var tlas := keep(rd.tlas_create(3, RenderingDevice.ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT))
	var transform := Transform3D(Basis(Vector3.FORWARD, 0.4).scaled(Vector3(1.2, 0.8, 1)), Vector3(1, 2, 3))
	var a := instance(blas, 37, 1, transform)
	var b := instance(blas, 71, 2, Transform3D(transform.basis, Vector3(1, 2, -1)))
	var placeholder := RDAccelerationStructureInstance.new()
	# Rebuild the same TLAS and dispatch through the same uniform set. Change
	# transforms/masks; retain placeholders so built-in instance IDs stay stable.
	var ray_buffer := keep(rd.storage_buffer_create(6 * 32))
	var results := keep(rd.storage_buffer_create(6 * 32))
	var uniforms: Array[RDUniform] = []
	for j in 3:
		var u := RDUniform.new()
		u.binding = j
		u.uniform_type = RenderingDevice.UNIFORM_TYPE_ACCELERATION_STRUCTURE if j == 0 else RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		u.add_id([tlas, ray_buffer, results][j])
		uniforms.append(u)
	var uniform_set: RID
	for iteration in 3:
		a.transform.origin.z = 3 + iteration
		assert(rd.tlas_build(tlas, [placeholder, a, b]) == OK)
		if not uniform_set.is_valid(): uniform_set = keep(rd.uniform_set_create(uniforms, shader, 0))
		var rays := PackedFloat32Array()
		for j in 6:
			var p := transform * Vector3(3 if j == 1 else 0, -1.0 / 3.0, 0)
			if j == 5: p.x += 100
			rays.append_array(PackedFloat32Array([p.x, p.y, -10 if j == 3 else 10, 2 if j == 2 else (4 if j == 4 else 1), 0, 0, 1 if j == 3 else -1, 0]))
		var bytes := rays.to_byte_array()
		assert(rd.buffer_update(ray_buffer, 0, bytes.size(), bytes) == OK)
		var list := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(list, pipeline)
		rd.compute_list_bind_uniform_set(list, uniform_set, 0)
		rd.compute_list_dispatch(list, 6, 1, 1)
		rd.compute_list_end()
		rd.submit()
		rd.sync()
		var data := rd.buffer_get_data(results)
		for j in 6:
			assert(data.decode_float(j * 32 + 28) == 0.0, "Committed intersection must be empty before traversal")
			var ids: Array = []
			for k in 4: ids.append(data.decode_u32(j * 32 + k * 4))
			var expected := [4294967295, 4294967295, 4294967295, 4294967295] if j >= 4 else [71 if j == 2 else 37, 2 if j == 2 else 1, 1 if j == 3 else 0, 1 if j == 1 else 0]
			assert(ids == expected, str([iteration, j, ids, expected]))
			if j < 4:
				var distance := 11.0 if j == 2 else (11.0 + iteration if j == 3 else 7.0 - iteration)
				assert(absf(data.decode_float(j * 32 + 16) - distance) < 0.0001)
				assert(absf(data.decode_float(j * 32 + 20) - 1.0 / 3.0) < 0.0001)
				assert(absf(data.decode_float(j * 32 + 24) - 1.0 / 3.0) < 0.0001)
			checks += 1
	resources.reverse()
	for rid in resources:
		rd.free_rid(rid)
	rd.free()
	print("[METAL_RAY_QUERY] passed ", checks, " transformed/indexed/masked/rebuilt ray checks; candidates=", candidates)
	quit()
