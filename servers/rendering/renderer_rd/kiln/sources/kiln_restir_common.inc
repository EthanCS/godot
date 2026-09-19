// RTDGI math adapted from Kajiya (MIT), assets/shaders/rtdgi and inc/math.hlsl.
layout(set = 0, binding = 1) uniform sampler2D restir_depth;
layout(set = 0, binding = 2) uniform sampler2D restir_normal;
layout(set = 0, binding = 3) uniform sampler2D restir_blue_noise;
const float RESTIR_SKY_DISTANCE = 1e4;
const float RESTIR_GOLDEN_ANGLE = 2.39996322972865332;
bool restir_inside(ivec2 px, ivec2 size) {
	return all(greaterThanEqual(px, ivec2(0))) && all(lessThan(px, size));
}
ivec2 restir_half_size() { return (ivec2(p.size_frame.xy) + 1) / 2; }
ivec2 restir_hi(ivec2 px) {
	const ivec2 offsets[4] = ivec2[4](ivec2(0, 0), ivec2(1, 1), ivec2(1, 0), ivec2(0, 1));
	return min(px * 2 + offsets[int(p.size_frame.z) & 3], ivec2(p.size_frame.xy) - 1);
}
vec2 restir_uv(ivec2 px) { return (vec2(px) + 0.5) / p.size_frame.xy; }
vec3 restir_normal_vs(ivec2 px) { return safe_normalize(texelFetch(restir_normal, px, 0).xyz); }
vec3 restir_normal_ws(ivec2 px) {
    vec3 n = mat3(p.inv_view) * restir_normal_vs(px);
    return kiln_unpack_normal_11_10_11(floatBitsToUint(kiln_pack_normal_11_10_11(n)));
}
vec3 restir_half_normal_ws(ivec2 px) {
    vec3 n = safe_normalize(mat3(p.view) * restir_normal_ws(restir_hi(px)));
    return mat3(p.inv_view) * (round(clamp(n, vec3(-1), vec3(1)) * 127.0) / 127.0);
}
uint restir_payload(ivec2 px) { return uint(px.x) | (uint(px.y) << 16u); }
ivec2 restir_pixel(uint payload) { return ivec2(payload & 0xffffu, payload >> 16u); }
vec4 restir_blue(ivec2 px, uint frame) {
	ivec2 offset = ivec2(kiln_r2_sequence(frame) * 256.0);
	return texelFetch(restir_blue_noise, (px + offset) & 255, 0) * (255.0 / 256.0) + (0.5 / 256.0);
}
mat3 restir_basis(vec3 n) {
	if (n.z < 0.0) {
		float a = 1.0 / (1.0 - n.z), b = n.x * n.y * a;
		return mat3(vec3(1.0 - n.x * n.x * a, -b, n.x), vec3(b, n.y * n.y * a - 1.0, -n.y), n);
	}
	float a = 1.0 / (1.0 + n.z), b = -n.x * n.y * a;
	return mat3(vec3(1.0 - n.x * n.x * a, b, -n.x), vec3(b, 1.0 - n.y * n.y * a, -n.y), n);
}
vec3 restir_direction(ivec2 px, vec3 normal) {
	vec2 u = restir_blue(px, uint(p.size_frame.z)).xy;
	float phi = u.y * 2.0 * PI, z = 1.0 - u.x, r = sqrt(1.0 - z * z);
	return restir_basis(normal) * vec3(cos(phi) * r, sin(phi) * r, z);
}
vec3 restir_origin(ivec2 hi, float depth) {
	vec3 v = view_position(restir_uv(hi), depth);
	vec3 w = (p.inv_view * vec4(v, 1.0)).xyz;
	return w - safe_normalize(w - p.inv_view[3].xyz) * (length(v) + length(w)) * 1e-4;
}
vec3 restir_project(vec3 pos) {
	vec4 clip = p.projection * p.view * vec4(pos, 1.0);
	return clip.xyz / clip.w;
}
float restir_relative_depth(float a, float b) { return abs(max(1e-20, a) / max(1e-20, b) - 1.0); }
float restir_target(vec3 radiance, vec3 direction, vec3 normal) {
	return max(1e-3, luminance(radiance)) * max(0.0, dot(direction, normal));
}
float restir_jacobian(float old_distance, float new_distance, vec4 hit_normal_dot, vec3 direction) {
	float ratio = clamp(old_distance / max(1e-8, new_distance), 1e-4, 1e4);
	return ratio * ratio * clamp(-dot(hit_normal_dot.xyz, direction) / max(1e-8, hit_normal_dot.w), 0.0, 1e4);
}
// Finite-thickness screen visibility from the reference temporal/spatial reuse.
float restir_visibility(vec3 origin, vec3 hit, float receiver_distance, vec2 uv, int max_taps, bool half_res) {
	vec3 delta = hit - origin;
	vec3 end = origin + delta * min(1.0, receiver_distance / max(1e-8, length(delta)));
	vec2 end_uv = restir_project(end).xy * 0.5 + 0.5;
	int taps = min(max_taps, int(floor(length((end_uv - uv) * p.size_frame.xy * (half_res ? 0.5 : 1.0)) / 2.0)));
	float visibility = 1.0;
	for (int k = 0; k < taps; ++k) {
		vec3 clip = restir_project(mix(origin, end, (float(k) + 0.5) / float(taps)));
		ivec2 px = ivec2((clip.xy * 0.5 + 0.5) * p.size_frame.xy);
		if (!restir_inside(px, ivec2(p.size_frame.xy))) continue;
		if (half_res) px = restir_hi(px / 2);
		float depth = texelFetch(restir_depth, px, 0).r;
		if (depth > clip.z * 1.001) visibility *= smoothstep(0.0, 0.05, restir_relative_depth(clip.z, depth));
	}
	return visibility;
}
