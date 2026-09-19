// Godot resource adapter for the retained Kajiya compute kernels (MIT).
#include "inc/frame_constants.hlsl"
#include "inc/gbuffer.hlsl"
#include "offsets.hlsl"
#include "inc/samplers.hlsl"
[[vk::binding(1)]] Texture2D<float> kiln_depth;
[[vk::binding(2)]] Texture2D<float4> kiln_normal;
[[vk::binding(6)]] Texture2D<float4> kiln_albedo;
[[vk::binding(7)]] Texture2D<uint2> kiln_surface;
#define kiln_input_tex_size kiln_gbuffer_tex_size
#define kiln_gbuffer_tex_size float4(p.size_frame.xy, 1.0 / p.size_frame.xy)
#ifdef KILN_HALF_OUTPUT
#define kiln_output_tex_size float4(p.size_frame.xy * 0.5, 2.0 / p.size_frame.xy)
#else
#define kiln_output_tex_size kiln_gbuffer_tex_size
#endif
float4 kiln_gbuffer_packed(int2 px) {
    float4 m = kiln_albedo[px], n = kiln_normal[px];
    GbufferData g = GbufferData::create_zero();
    g.albedo = m.rgb;
    g.metalness = m.a;
    g.normal = normalize(direction_view_to_world(n.xyz));
    // Godot stores perceptual roughness; Kajiya's BRDF uses GGX alpha.
    g.roughness = max(1e-4, n.a * n.a);
    return asfloat(g.pack().data0);
}
int2 kiln_half_pixel(int2 px) {
    const int2 offsets[4] = { int2(0, 0), int2(1, 1), int2(1, 0), int2(0, 1) };
    return px * 2 + offsets[uint(p.size_frame.z) & 3];
}
float3 kiln_half_normal(int2 px) {
    float3 n = normalize(direction_world_to_view(GbufferDataPacked::from_uint4(asuint(kiln_gbuffer_packed(kiln_half_pixel(px)))).unpack_normal()));
    return round(clamp(n, -1.0, 1.0) * 127.0) / 127.0;
}
float kiln_half_depth(int2 px) { return kiln_depth[kiln_half_pixel(px)]; }
float3 kiln_geometric_normal(int2 px) {
    uint packed = kiln_surface[px].y;
    float2 f = clamp(float2(int(packed << 16) >> 16, int(packed) >> 16) / 32767.0, -1.0, 1.0);
    float3 n = float3(f, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    return round((normalize(n) * 0.5 + 0.5) * 1023.0) / 1023.0;
}
