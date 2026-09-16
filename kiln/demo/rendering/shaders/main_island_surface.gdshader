shader_type spatial;
render_mode kiln_surface, diffuse_burley, cull_disabled, ambient_light_disabled;
// Linear authored values from the original island material manifest.
// Kiln indirect illumination belongs to the engine, never to material EMISSION.
// Opt in to native rigid, solid, uniform GI transport.
uniform bool kiln_uniform_transport = true;
uniform vec3 tint_linear = vec3(1.0);
uniform float constant_roughness = 0.8;
uniform float metalness = 0.0;
uniform float transmission = 0.0;
uniform vec3 authored_emission = vec3(0.0);
void fragment() {
    ALBEDO = tint_linear;
    ROUGHNESS = constant_roughness;
    METALLIC = metalness;
    EMISSION = authored_emission;
    BACKLIGHT = vec3(transmission);
}
