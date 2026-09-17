// The cache follows the original geometric surface. Shading-normal maps remain
// in the deferred material buffer and must not fragment world-space coverage.
layout(set = 0, binding = 31) uniform usampler2D surface_geometry;
vec3 surfel_view_normal(sampler2D normals, sampler2D depths, ivec2 pixel) {
	if (p.engine_state.y < 0.5) {
		return receiver_view_normal(normals, depths, pixel);
	}
	uint packed = texelFetch(surface_geometry, pixel, 0).g;
	vec2 oct = unpackSnorm2x16(packed);
	vec3 n = vec3(oct, 1.0 - abs(oct.x) - abs(oct.y));
	n.xy += mix(vec2(1), vec2(-1), greaterThanEqual(n.xy, vec2(0))) * max(-n.z, 0.0);
	return safe_normalize(n);
}
