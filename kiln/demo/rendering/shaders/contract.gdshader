shader_type spatial;
render_mode kiln_surface, cull_disabled, ambient_light_disabled;
uniform sampler2D checker : source_color;
uniform sampler2D normal_tex : hint_normal;
uniform bool cutout = false;
uniform bool mapped_normal = false;
uniform bool colored_vertices = false;
void fragment() {
    vec4 texel = texture(checker, UV * 4.0);
    ALBEDO = texel.rgb * (colored_vertices ? COLOR.rgb : vec3(1));
    ROUGHNESS = 0.65;
    if (cutout) {
        ALPHA = step(0.0, sin(UV.x * 80.0) * sin(UV.y * 80.0));
        ALPHA_SCISSOR_THRESHOLD = 0.5;
    }
    if (mapped_normal) { NORMAL_MAP = texture(normal_tex, UV * 8.0).rgb; }
}
