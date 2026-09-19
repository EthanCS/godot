// Kajiya rt/gbuffer.rchit.hlsl uses a flat geometric normal at ray hits.
// Imported Godot triangle winding is the reverse of the reference glTF winding.
vec3 kiln_ray_normal(int id) {
    Triangle t = world_triangle(id);
    vec3 n = -safe_normalize(cross(t.p1.xyz, t.p2.xyz));
    return kiln_unpack_normal_11_10_11(floatBitsToUint(kiln_pack_normal_11_10_11(n)));
}
vec4 kiln_ray_material(int id, vec3 position) {
    vec4 value = hit_base_roughness(id, position);
    value.rgb = pow(floor(sqrt(clamp(value.rgb, vec3(0), vec3(1))) * 255.0) / 255.0, vec3(2));
    value.w = max(1e-4, value.w * value.w);
    // GbufferData::pack rounds perceptual roughness through binary16.
    value.w = pow(unpackHalf2x16(packHalf2x16(vec2(sqrt(value.w), 0))).x, 2.0);
    return value;
}
