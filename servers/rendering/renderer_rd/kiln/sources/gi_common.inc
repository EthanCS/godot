// Shared std140 contract. Matrices are Godot's already-corrected GPU projections.
layout(set=0,binding=0,std140) uniform Parameters {
    mat4 projection;
    mat4 inv_projection;
    mat4 inv_view;
    mat4 view;
    mat4 previous_view_projection;
    vec4 size_frame;       // full width, height, frame, history valid
    vec4 gi;               // AO radius, abrupt lighting response [0,1], stationary batch (-1 moving), convergence batches
    vec4 quality;          // rays, history reprojection, AO quality, diffuse reconstruction enabled
    vec4 voxel_min;        // source bounds minimum xyz; w: primary surfel ray budget (0 unlimited)
    vec4 voxel_size;       // source scene bounds size xyz, surfel pool capacity
    vec4 voxel_state;      // x: triangle count; y: rough reflection checkerboard; z: NRD off/full/checkerboard (0/1/2); w: epoch
    vec4 sun_direction;    // direction TO sun, energy
    vec4 sun_color;        // linear RGB, sky energy
    vec4 sky_color;        // linear RGB, irradiance sharing enabled
    vec4 debug;            // debug mode, display disk radius scale, illumination display gain, reserved
    vec4 indirect_tint;    // original linear color_library.indirect_light_tint, w = pixel albedo available
    vec4 sky_high;         // RGB, mode: 0 constant / 1 legacy / 2 rendering/sky
    vec4 fake_light_color; // legacy lobe RGB; w = project sky solar halo energy
    vec4 fake_light_direction; // sun direction; w = project sky time of day
    vec4 fake_light2_color; // legacy second lobe; project sky cloud RGB / coverage
    vec4 fake_light2_direction; // legacy direction; x = project sky saturation
    vec4 ground_escape;    // original theme2 downward ray miss contribution, w = solid BRDF compensation
    vec4 source_bvh_state; // static nodes, static triangles, GI enabled, continuous lighting response [0,1]
    vec4 dynamic_scene;    // dynamic nodes, dynamic triangles, emissive triangles, total sampling power
    mat4 previous_inverse_view_projection;
    vec4 engine_state; // x: signed material normal; y: G-buffer surface available; z: multibounce; w: dynamic rebuild
} p;

const float PI=3.14159265358979323846;
vec3 safe_normalize(vec3 v) { return v * inversesqrt(max(dot(v,v),1e-16)); }
vec3 receiver_view_normal(sampler2D normals,sampler2D depths,ivec2 pixel){
    vec3 value=texelFetch(normals,pixel,0).xyz;
    return safe_normalize(p.engine_state.x>.5?value:value*2.0-1.0);
}
float receiver_roughness(sampler2D normals, ivec2 pixel) {
    float r = texelFetch(normals, pixel, 0).a;
    // Forward+ packs the dynamic/static flag into its normal prepass alpha.
    return clamp(p.engine_state.x > 0.5 ? r : min(r, 1.0-r) * (255.0/127.0), 0.0, 1.0);
}
vec3 view_position(vec2 uv,float depth) {
    vec4 point=p.inv_projection*vec4(uv*2.0-1.0,depth,1.0);
    return point.xyz/point.w;
}
vec3 world_position(vec2 uv,float depth) {return (p.inv_view*vec4(view_position(uv,depth),1.0)).xyz;}
vec2 project_view(vec3 v) {
    vec4 c=p.projection*vec4(v,1.0);
    return c.xy/c.w*0.5+0.5;
}
float luminance(vec3 v) {return dot(v,vec3(.2126,.7152,.0722));}
// @PROJECT_SKY@
vec3 environment_radiance(vec3 ray){
    if(p.sky_high.w<.5)return p.sky_color.rgb*p.sun_color.w;
    if(ray.y<=0.0)return p.ground_escape.rgb*p.sun_color.w;
    // Sky rays still travel through the BVH. The solar disc is sampled only
    // by the direct light, avoiding double sun energy and tiny-disc fireflies.
    if(p.sky_high.w>1.5)return project_sky_radiance(ray,p.sun_direction.xyz,
        p.sky_color.rgb,p.sky_high.rgb,p.fake_light_color.w,p.fake_light_direction.w,
        p.fake_light2_direction.x,p.fake_light2_color.rgb,p.fake_light2_color.w)*p.sun_color.w;
    float alignment=dot(ray,p.fake_light_direction.xyz);
    vec3 directional_tint=mix(mix(vec3(.2,.4,1),vec3(1,.4,.2),alignment*.5+.5),vec3(1),ray.y*ray.y);
    vec3 fake=(p.fake_light_color.rgb*pow(max(0.0,alignment),12.0)
        +p.fake_light2_color.rgb*pow(max(0.0,dot(ray,p.fake_light2_direction.xyz)),12.0))*3.0;
    vec3 tint=max(vec3(0),vec3(1)-p.indirect_tint.rgb*.9200000166893005);
    float sky_mix=pow(clamp(1.0-pow(1.0-clamp(ray.y+.2,0.0,1.0),14.0),0.0,1.0),.6499999761581421);
    vec3 sky=mix(p.sky_color.rgb,p.sky_high.rgb,sky_mix)*1.0999999046325684
        *directional_tint/max(luminance(directional_tint),1e-6);
    sky+=max(vec3(0),mix(tint/max(luminance(tint),1e-6)*luminance(fake),fake,.8547008633613586)*.8333333134651184);
    return sky*p.sun_color.w;
}
vec3 cosine_direction(vec3 normal,vec2 xi) {
    float r=sqrt(xi.x), phi=2.0*PI*xi.y;
    vec3 tangent=safe_normalize(cross(abs(normal.y)<.95?vec3(0,1,0):vec3(1,0,0),normal));
    vec3 bitangent=cross(normal,tangent);
    return tangent*(r*cos(phi))+bitangent*(r*sin(phi))+normal*sqrt(max(0.0,1.0-xi.x));
}
uint hilbert_index(uvec2 pixel) {
    uint x=pixel.x&63u,y=pixel.y&63u,index=0u;
    for(uint s=32u;s>0u;s/=2u){
        uint rx=uint((x&s)>0u),ry=uint((y&s)>0u);
        index+=s*s*((3u*rx)^ry);
        if(ry==0u){if(rx==1u){x=63u-x;y=63u-y;}uint tmp=x;x=y;y=tmp;}
    }
    return index;
}
