shader_type sky;
#include "res://rendering/sky/sky_radiance.gdshaderinc"
uniform vec3 horizon = vec3(0.50, 0.58, 0.70);
uniform vec3 zenith = vec3(0.18, 0.28, 0.43);
uniform float energy = 1.0;
uniform float halo_strength = 0.035;
uniform float time_of_day = 0.175;
uniform float saturation = 1.12;
uniform vec3 cloud_color = vec3(0.78, 0.81, 0.85);
uniform float cloud_coverage = 0.40;
void sky() {
    vec3 ray = normalize(EYEDIR);
    vec3 sun = LIGHT0_ENABLED ? LIGHT0_DIRECTION : vec3(0.0, 1.0, 0.0);
    float halo = LIGHT0_ENABLED ? halo_strength * LIGHT0_ENERGY : 0.0;
    COLOR = energy * (project_sky_radiance(ray, sun, horizon, zenith,
        halo, time_of_day, saturation, cloud_color, cloud_coverage)
        + project_sky_disc(ray, sun, halo, saturation, cloud_coverage));
}
