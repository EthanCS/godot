// Descriptor/workgroup metadata only. The Metal container must replace this
// entry point with handwritten surfel_specular.metal; it is never dispatched.
// Keep the field order identical to Kiln's shared Parameters and storage ABI.
#[compute]
#version 460
#VERSION_DEFINES
#extension GL_EXT_ray_query : require
layout(local_size_x=16,local_size_y=2,local_size_z=2) in;
layout(constant_id=0) const uint REFLECTION_RAY_COUNT=2u;
layout(set=0,binding=0,std140) uniform Parameters {
    mat4 projection, inv_projection, inv_view, view, previous_view_projection;
    vec4 size_frame, gi, quality, voxel_min, voxel_size, voxel_state;
    vec4 sun_direction, sun_color, sky_color, debug, indirect_tint, sky_high;
    vec4 fake_light_color, fake_light_direction, fake_light2_color, fake_light2_direction;
    vec4 ground_escape, source_bvh_state, dynamic_scene;
    mat4 previous_inverse_view_projection;
    vec4 engine_state;
} p;
struct Triangle { vec4 p0; vec4 p1; vec4 p2; vec4 albedo; vec4 emission; };
struct TextureCoordinates { vec4 ab; vec4 c_page_repeat; };
struct Surfel {
    vec4 position_radius; vec4 normal_age; vec4 irradiance_samples;
    vec4 short_mean_vbbr; vec4 variance_inconsistency; uvec4 anchor;
    vec4 barycentric; vec4 local_normal;
};
layout(set=0,binding=1) uniform sampler2D depth;
layout(set=0,binding=2) uniform sampler2D normals;
layout(set=0,binding=4,std430) readonly buffer Nodes { vec4 nodes[]; };
layout(set=0,binding=5,std430) readonly buffer Triangles { Triangle triangles[]; };
layout(set=0,binding=6,rgba16f) uniform writeonly image2D specular_base;
layout(set=0,binding=7,rgba16f) uniform writeonly image2D specular_fresnel;
layout(set=0,binding=16,std430) readonly buffer DynamicNodes { vec4 dynamic_nodes[]; };
layout(set=0,binding=17,std430) readonly buffer DynamicTriangles { Triangle dynamic_triangles[]; };
layout(set=0,binding=18,std430) readonly buffer Emitters { vec4 emitters[]; };
layout(set=0,binding=20,std430) readonly buffer Surfels { Surfel surfels[]; };
layout(set=0,binding=21,std430) readonly buffer CellHeads { uvec2 cell_heads[]; };
layout(set=0,binding=22,std430) readonly buffer CellLinks { uint cell_links[]; };
layout(set=0,binding=23,std430) readonly buffer FreeSlots { uint free_slots[]; };
layout(set=0,binding=24,std430) readonly buffer Counters { uint counters[]; };
layout(set=0,binding=25,std430) readonly buffer LocalLights { vec4 local_lights[]; };
layout(set=0,binding=26,std430) readonly buffer LightGrid { uint light_grid[]; };
layout(set=0,binding=27) uniform accelerationStructureEXT scene_tlas;
layout(set=0,binding=31) uniform usampler2D surface_geometry;
layout(set=0,binding=32) uniform sampler2DArray ray_albedo;
layout(set=0,binding=33,std430) readonly buffer StaticUVs { TextureCoordinates static_uvs[]; };
layout(set=0,binding=34,std430) readonly buffer DynamicUVs { TextureCoordinates dynamic_uvs[]; };
layout(set=0,binding=35,std430) readonly buffer GridSums { uint grid_sums[]; };
void main() {
    // Deliberately unmistakable output if this metadata-only shader is ever
    // incorrectly dispatched on a backend that does not install the native MSL.
    imageStore(specular_base, ivec2(gl_GlobalInvocationID.xy), vec4(float(REFLECTION_RAY_COUNT), 0, 1000, 0));
}
