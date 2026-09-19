#ifndef FRAME_CONSTANTS_HLSL
#define FRAME_CONSTANTS_HLSL
#include "uv.hlsl"
struct Parameters {
    column_major float4x4 projection;
    column_major float4x4 inv_projection;
    column_major float4x4 inv_view;
    column_major float4x4 view;
    float4 size_frame;       // full width, height, frame, history valid
    float4 sun_direction;    // direction TO sun, energy
    float4 sun_color;        // linear RGB, sky energy
    float4 source_bvh_state; // static nodes, static triangles, GI enabled, continuous lighting response [0,1]
    float4 dynamic_scene;    // dynamic nodes, dynamic triangles, emissive triangles, total sampling power
    float4 kiln_prev_eye; // xyz: previous eye position, w: sun size multiplier
    float4 lighting; // x: sun angular radius cos, y: ev shift, z: ReSTIR path enabled, w: previous frame index
    float4 post; // sampling offset in pixels, frame delta seconds, reserved
    column_major float4x4 unjittered_projection;
    column_major float4x4 inverse_unjittered_projection;
    column_major float4x4 previous_unjittered_view_projection;
    column_major float4x4 previous_inverse_projection;
    column_major float4x4 previous_inverse_view;
};
[[vk::binding(0)]] ConstantBuffer<Parameters> p;
struct ViewConstants {
    float4x4 view_to_clip, clip_to_view, view_to_sample, sample_to_view;
    float4x4 world_to_view, view_to_world, clip_to_prev_clip;
    float4x4 prev_view_to_prev_world;
    float2 sample_offset_pixels;
};
struct FrameConstants { ViewConstants view_constants; uint frame_index; float delta_time_seconds; };
FrameConstants kiln_frame_constants() {
    FrameConstants f;
    f.frame_index = uint(p.size_frame.z);
    f.delta_time_seconds = p.post.z;
    f.view_constants.sample_offset_pixels = p.post.xy;
    f.view_constants.view_to_clip = p.unjittered_projection;
    f.view_constants.view_to_sample = p.projection;
    f.view_constants.clip_to_view = p.inverse_unjittered_projection;
    f.view_constants.sample_to_view = p.inv_projection;
    f.view_constants.world_to_view = p.view;
    f.view_constants.view_to_world = p.inv_view;
    f.view_constants.clip_to_prev_clip = mul(p.previous_unjittered_view_projection, mul(p.inv_view, p.inverse_unjittered_projection));
    f.view_constants.prev_view_to_prev_world = p.previous_inverse_view;
    return f;
}
#define frame_constants kiln_frame_constants()
struct ViewRayContext {
    float4 ray_dir_cs;
    float4 ray_dir_vs_h;
    float4 ray_dir_ws_h;

    float4 ray_origin_cs;
    float4 ray_origin_vs_h;
    float4 ray_origin_ws_h;

    float4 ray_hit_cs;
    float4 ray_hit_vs_h;
    float4 ray_hit_ws_h;

    float3 ray_dir_vs() {
        return normalize(ray_dir_vs_h.xyz);
    }

    float3 ray_dir_ws() {
        return normalize(ray_dir_ws_h.xyz);
    }

    // TODO: might need previous frame versions of those

    float3 ray_origin_vs() {
        return ray_origin_vs_h.xyz / ray_origin_vs_h.w;
    }

    float3 ray_origin_ws() {
        return ray_origin_ws_h.xyz / ray_origin_ws_h.w;
    }

    float3 ray_hit_vs() {
        return ray_hit_vs_h.xyz / ray_hit_vs_h.w;
    }

    float3 ray_hit_ws() {
        return ray_hit_ws_h.xyz / ray_hit_ws_h.w;
    }

    // A biased position from which secondary rays can be shot without too much acne or leaking
    float3 biased_secondary_ray_origin_ws() {
        return ray_hit_ws() - ray_dir_ws() * (length(ray_hit_vs()) + length(ray_hit_ws())) * 1e-4;
    }

    static ViewRayContext from_uv(float2 uv) {
        ViewConstants view_constants = frame_constants.view_constants;

        ViewRayContext res;
        res.ray_dir_cs = float4(uv_to_cs(uv), 0.0, 1.0);
        res.ray_dir_vs_h = mul(view_constants.sample_to_view, res.ray_dir_cs);
        // Kajiya uses an infinite reverse-Z projection. Godot's finite far
        // plane must not turn this direction into a translated world point.
        res.ray_dir_vs_h.w = 0.0;
        res.ray_dir_ws_h = mul(view_constants.view_to_world, res.ray_dir_vs_h);

        res.ray_origin_cs = float4(uv_to_cs(uv), 1.0, 1.0);
        res.ray_origin_vs_h = mul(view_constants.sample_to_view, res.ray_origin_cs);
        res.ray_origin_ws_h = mul(view_constants.view_to_world, res.ray_origin_vs_h);

        return res;
    }

    static ViewRayContext from_uv_and_depth(float2 uv, float depth) {
        ViewConstants view_constants = frame_constants.view_constants;

        ViewRayContext res;
        res.ray_dir_cs = float4(uv_to_cs(uv), 0.0, 1.0);
        res.ray_dir_vs_h = mul(view_constants.sample_to_view, res.ray_dir_cs);
        res.ray_dir_vs_h.w = 0.0;
        res.ray_dir_ws_h = mul(view_constants.view_to_world, res.ray_dir_vs_h);

        res.ray_origin_cs = float4(uv_to_cs(uv), 1.0, 1.0);
        res.ray_origin_vs_h = mul(view_constants.sample_to_view, res.ray_origin_cs);
        res.ray_origin_ws_h = mul(view_constants.view_to_world, res.ray_origin_vs_h);

        res.ray_hit_cs = float4(uv_to_cs(uv), depth, 1.0);
        res.ray_hit_vs_h = mul(view_constants.sample_to_view, res.ray_hit_cs);
        res.ray_hit_ws_h = mul(view_constants.view_to_world, res.ray_hit_vs_h);

        return res;
    }
};

float3 get_eye_position() {
    float4 eye_pos_h = mul(frame_constants.view_constants.view_to_world, float4(0, 0, 0, 1));
    return eye_pos_h.xyz / eye_pos_h.w;
}

float3 get_prev_eye_position() {
    float4 eye_pos_h = mul(frame_constants.view_constants.prev_view_to_prev_world, float4(0, 0, 0, 1));
    return eye_pos_h.xyz / eye_pos_h.w;
}

float depth_to_view_z(float depth) {
    float4 v = mul(p.inv_projection, float4(0, 0, depth, 1)); return v.z / v.w;
}

float3 direction_view_to_world(float3 v) {
    return mul(frame_constants.view_constants.view_to_world, float4(v, 0)).xyz;
}

float3 direction_world_to_view(float3 v) {
    return mul(frame_constants.view_constants.world_to_view, float4(v, 0)).xyz;
}

float3 position_world_to_view(float3 v) {
    return mul(frame_constants.view_constants.world_to_view, float4(v, 1)).xyz;
}

float3 position_view_to_world(float3 v) {
    return mul(frame_constants.view_constants.view_to_world, float4(v, 1)).xyz;
}

float3 position_world_to_clip(float3 v) {
    float4 p = mul(frame_constants.view_constants.world_to_view, float4(v, 1));
    p = mul(frame_constants.view_constants.view_to_clip, p);
    return p.xyz / p.w;
}

float3 position_world_to_sample(float3 v) {
    float4 p = mul(frame_constants.view_constants.world_to_view, float4(v, 1));
    p = mul(frame_constants.view_constants.view_to_sample, p);
    return p.xyz / p.w;
}


#endif
