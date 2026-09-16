shader_type spatial;
render_mode cull_disabled, depth_draw_never;
void fragment() {
    ALBEDO = vec3(0.12, 0.65, 0.8);
    ROUGHNESS = 0.3;
    ALPHA = 0.35;
}
