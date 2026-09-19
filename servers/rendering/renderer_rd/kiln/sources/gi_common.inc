// Shared std140 contract. Matrices are Godot's already-corrected GPU projections.
layout(set=0,binding=0,std140) uniform Parameters {
    mat4 projection;
    mat4 inv_projection;
    mat4 inv_view;
    mat4 view;
    vec4 size_frame;       // full width, height, frame, history valid
    vec4 sun_direction;    // direction TO sun, energy
    vec4 sun_color;        // linear RGB, sky energy
    vec4 source_bvh_state; // static nodes, static triangles, GI enabled, continuous lighting response [0,1]
    vec4 dynamic_scene;    // dynamic nodes, dynamic triangles, emissive triangles, total sampling power
    vec4 kiln_prev_eye; // xyz: previous eye position, w: sun size multiplier
    vec4 lighting; // x: sun angular radius cos, y: ev shift, z: ReSTIR path enabled, w: previous frame index
    vec4 post; // sampling offset in pixels, frame delta seconds, reserved
    mat4 unjittered_projection;
    mat4 inverse_unjittered_projection;
    mat4 previous_unjittered_view_projection;
    mat4 previous_inverse_projection;
    mat4 previous_inverse_view;
} p;

const float PI=3.14159265358979323846;
vec3 safe_normalize(vec3 v) { return v * inversesqrt(max(dot(v,v),1e-16)); }
vec3 view_position(vec2 uv,float depth) {
    vec4 point=p.inv_projection*vec4(uv*2.0-1.0,depth,1.0);
    return point.xyz/point.w;
}
vec3 world_position(vec2 uv,float depth) {return (p.inv_view*vec4(view_position(uv,depth),1.0)).xyz;}
float luminance(vec3 v) {return dot(v,vec3(.2126,.7152,.0722));}
