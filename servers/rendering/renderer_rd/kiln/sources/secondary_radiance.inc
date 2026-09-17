vec3 reflected_radiance(vec3 origin, vec3 direction, inout uint seed, out float hit_distance) {
	int triangle;
	vec3 normal;
	if (!world_trace(origin, direction, 1000.0, false, triangle, hit_distance, normal)) {
		hit_distance = 1000.0;
		return environment_radiance(direction);
	}
	Triangle t = world_triangle(triangle);
	vec3 hit = origin + direction * hit_distance;
	vec3 incident = vec3(0);
	float nl = max(dot(normal, p.sun_direction.xyz), 0.0);
	if (nl > 0.0 && p.sun_direction.w > 0.0 && !occluded(hit + normal * 0.025, p.sun_direction.xyz)) {
		incident += p.sun_color.rgb * (p.sun_direction.w * nl / PI);
	}
	incident += local_light_sample(hit, normal, random_float(seed));
	vec4 cached = surfel_gather(hit, normal, false);
	if (cached.a > 0.1
#ifdef STAGE_NRD_DIFFUSE
			&& p.engine_state.z > 0.5
#endif
	) {
		incident += cached.rgb;
	} else {
		// Offscreen, newly exposed surfaces have no camera-generated surfels yet.
		// Trace their illumination instead of returning a black reflection.
		incident += sky_at_surface(hit, normal, random_pair(seed));
		vec3 emitter_direction;
		vec3 xi = vec3(random_pair(seed), random_float(seed));
		incident += emitter_sample(hit, normal, xi, emitter_direction) * max(dot(normal, emitter_direction), 0.0) / PI;
	}
	// Unlike diffuse cache rays, these BRDF samples include hit emission directly.
	// There is no second emitter estimator at the receiver to double count it.
#ifdef STAGE_NRD_DIFFUSE
	// Primary diffuse estimates emission with explicit emitter sampling.
	return clamp(hit_albedo(triangle, hit), vec3(0), vec3(0.95)) * incident;
#else
	return t.emission.rgb + clamp(hit_albedo(triangle, hit), vec3(0), vec3(0.95)) * incident;
#endif
}
