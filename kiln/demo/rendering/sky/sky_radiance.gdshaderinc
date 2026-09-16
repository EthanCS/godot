// Shared by the visible sky and BVH ray misses. All colors are linear radiance.
// The elevation curve, solar disc and two halo profiles follow the local
// TinyGladeInverse/shaders/captured_sky.gdshader reference. Clouds are procedural.
float sky_luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 sky_saturate(vec3 c, float saturation) {
    return max(vec3(0.0), mix(vec3(sky_luminance(c)), c, saturation));
}
float sky_cloud_union(float a, float b) {
    float h = max(0.26 - abs(a - b), 0.0) / 0.26;
    return min(a, b) - h * h * 0.065;
}
float sky_clouds(vec3 ray, float coverage) {
    if (coverage <= 0.0 || ray.y < -0.12 || ray.y > 0.55) return 0.0;
    // Rounded cumulus silhouettes on a direction-space ring. Analytic lobes
    // avoid texture/float-hash seams and remain stable under camera motion.
    float cloud = 0.0;
    for (int i = 0; i < 12; i++) {
        float seed = fract(float(i) * 0.618033989 + 0.31);
        float angle = float(i) * 2.39996323;
        vec2 facing = vec2(cos(angle), sin(angle));
        if (dot(ray.xz, facing) < 0.86) continue;
        float width = mix(0.09, 0.19, seed) * mix(0.45, 1.4, coverage);
        float height = mix(0.022, 0.055, seed);
        vec2 uv = vec2(dot(ray.xz, vec2(-facing.y, facing.x)) / width,
            (ray.y - mix(0.035, 0.22, fract(seed * 3.7))) / height);
        float d = length(uv / vec2(1.1, 0.40)) - 1.0;
        d = sky_cloud_union(d, length((uv - vec2(-0.55, 0.25)) / vec2(0.47, 0.60)) - 1.0);
        d = sky_cloud_union(d, length((uv - vec2(0.0, 0.48)) / vec2(0.55, 0.90)) - 1.0);
        d = sky_cloud_union(d, length((uv - vec2(0.58, 0.20)) / vec2(0.42, 0.55)) - 1.0);
        d += sin(uv.x * 13.0 + sin(uv.y * 9.0)) * sin(uv.y * 11.0) * 0.025;
        cloud = max(cloud, (1.0 - smoothstep(-0.14, 0.16, d)) * smoothstep(0.0, 0.15, coverage));
    }
    return cloud * smoothstep(-0.12, 0.015, ray.y);
}
vec3 project_sky_radiance(vec3 ray, vec3 sun_direction, vec3 low, vec3 high,
        float halo_energy, float time_of_day, float saturation, vec3 cloud_color, float coverage) {
    float elevation = pow(clamp(1.0 - pow(1.0 - clamp(ray.y + 0.2, 0.0, 1.0), 14.0), 0.0, 1.0), 0.65);
    float night = 1.0 - (smoothstep(0.483, 0.505, time_of_day) - smoothstep(0.84, 0.91, time_of_day)) * 0.75;
    vec3 base = mix(low * 1.5, high, elevation) * night;
    float angle = acos(clamp(dot(sun_direction, ray), -1.0, 1.0));
    float distance_from_sun = max(0.0, angle - 0.0261799395);
    float halo_a = 0.5 + 25.0 * distance_from_sun;
    float halo_b = 1.0 + 5.0 * distance_from_sun;
    vec3 halo = vec3(1.0, 0.65, 0.2) * (5.0 / (halo_a * halo_a))
        + vec3(1.0, 0.75, 0.6) * (0.8 / (halo_b * halo_b));
    float clouds = sky_clouds(ray, coverage);
    float cloud_light = 0.70 + 0.30 * smoothstep(-0.05, 0.28, ray.y);
    base = mix(base, cloud_color * cloud_light * night, clouds);
    return sky_saturate(base + halo * halo_energy * (1.0 - clouds * 0.85), saturation);
}
vec3 project_sky_disc(vec3 ray, vec3 sun_direction, float energy, float saturation, float coverage) {
    float angle = acos(clamp(dot(sun_direction, ray), -1.0, 1.0));
    float disc = 1.0 - smoothstep(0.0244346093, 0.0261799395, angle);
    return sky_saturate(vec3(1.0, 0.7, 0.2) * (disc * 30.0 * energy), saturation)
        * (1.0 - sky_clouds(ray, coverage));
}
