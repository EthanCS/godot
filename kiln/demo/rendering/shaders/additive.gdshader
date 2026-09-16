shader_type spatial;
render_mode unshaded, blend_add, cull_disabled, depth_draw_never;
void fragment() {
    ALBEDO = vec3(0.65, 0.16, 0.03);
    ALPHA = 0.25 * pow(max(0.0, 1.0 - length(UV - 0.5) * 2.0), 2.0);
}
