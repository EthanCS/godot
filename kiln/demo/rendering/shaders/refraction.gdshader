shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never;
uniform sampler2D completed_opaque : hint_screen_texture, filter_linear;
void fragment() {
    vec2 offset = (UV - 0.5) * 0.035;
    ALBEDO = texture(completed_opaque, SCREEN_UV + offset).rgb * vec3(0.85, 0.95, 1.0);
    ALPHA = 0.85;
}
