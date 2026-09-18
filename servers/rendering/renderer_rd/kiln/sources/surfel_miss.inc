// Cache misses are missing information, not proof of zero indirect light.
// Continue one diffuse segment, then use a valid cache or a bounded sky/emitter
// estimate. This retains lighting from offscreen surfaces without an unbounded
// path tracer or screen-resolution fallback rays.
vec3 surfel_miss_irradiance(vec3 position, vec3 normal, inout uint seed) {
	vec3 ed;
	vec3 ex = vec3(random_pair(seed), random_float(seed));
	vec3 value = emitter_sample(position, normal, ex, ed) * max(dot(normal, ed), 0.0) / PI;
	vec3 direction = cosine_direction(normal, random_pair(seed));
	if (p.engine_state.z < 0.5) {
		return value + sky_at_surface(position, normal, random_pair(seed));
	}
	vec3 origin = position + normal * 0.025;
	int triangle;
	float distance;
	vec3 hit_normal;
	if (!world_trace(origin, direction, 1000.0, false, triangle, distance, hit_normal)) {
		return value + environment_radiance(direction);
	}
	vec3 hit = origin + direction * distance;
	vec3 incident = local_light_sample(hit, hit_normal, random_float(seed));
	float nl = max(dot(hit_normal, p.sun_direction.xyz), 0.0);
	if (nl > 0.0 && p.sun_direction.w > 0.0 && !occluded(hit + hit_normal * 0.025, p.sun_direction.xyz)) {
		incident += p.sun_color.rgb * (p.sun_direction.w * nl / PI);
	}
	vec4 cached = surfel_gather(hit, hit_normal, false);
	if (cached.a > 0.1) {
		incident += cached.rgb;
	} else {
		incident += sky_at_surface(hit, hit_normal, random_pair(seed));
		ex = vec3(random_pair(seed), random_float(seed));
		incident += emitter_sample(hit, hit_normal, ex, ed) * max(dot(hit_normal, ed), 0.0) / PI;
	}
	// Emission at this hit is already represented by the emitter estimator at
	// the previous vertex. Adding it again would double the same transport path.
	return value + clamp(hit_albedo(triangle, hit), vec3(0), vec3(0.95)) * incident;
}
