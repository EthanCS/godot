// Shared std140 contract. Matrices are Godot's already-corrected GPU projections.
layout(set=0,binding=0,std140) uniform Parameters {
    mat4 projection;
    mat4 inv_projection;
    mat4 inv_view;
    mat4 view;
    mat4 previous_view_projection;
    vec4 size_frame;       // full width, height, frame, history valid
    vec4 gi;               // AO radius, intensity, stationary batch (-1 moving), convergence batches
    vec4 quality;          // rays, history reprojection (camera motion or TAA jitter), AO quality (0 off / 1..3), history position threshold
    vec4 voxel_min;        // min xyz, smallest cell size
    vec4 voxel_size;       // volume size xyz, world cache hash slots (power of two)
    vec4 voxel_state;      // triangle count, ready, exact publication cache available, world lighting epoch
    vec4 sun_direction;    // direction TO sun, energy
    vec4 sun_color;        // linear RGB, sky energy
    vec4 sky_color;        // linear RGB, fallback receiver albedo
    vec4 debug;            // debug mode, half width, half height, original SH mode
    vec4 indirect_tint;    // original linear color_library.indirect_light_tint, w = pixel albedo available
    vec4 sky_high;         // RGB, mode: 0 constant / 1 legacy / 2 rendering/sky
    vec4 fake_light_color; // legacy lobe RGB; w = project sky solar halo energy
    vec4 fake_light_direction; // sun direction; w = project sky time of day
    vec4 fake_light2_color; // legacy second lobe; project sky cloud RGB / coverage
    vec4 fake_light2_direction; // legacy direction; x = project sky saturation
    vec4 ground_escape;    // original theme2 downward ray miss contribution, w = solid BRDF compensation
    vec4 source_bvh_state; // static nodes, static triangles, GI enabled, lighting changing
    vec4 dynamic_scene;    // dynamic nodes, dynamic triangles, emissive triangles, total sampling power
    mat4 previous_inverse_view_projection;
    vec4 engine_state; // x: signed G-buffer normal (1), Forward+ encoded normal (0); y: receiver divisor
} p;

const float PI=3.14159265358979323846;
vec3 safe_normalize(vec3 v) { return v * inversesqrt(max(dot(v,v),1e-16)); }
vec3 receiver_view_normal(sampler2D normals,sampler2D depths,ivec2 pixel){
    vec3 value=texelFetch(normals,pixel,0).xyz;
    return safe_normalize(p.engine_state.x>.5?value:value*2.0-1.0);
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
float interleaved_noise(vec2 pixel) {
    // Exact constants recovered from gi_trace_gi_candidates.cs.hlsl.
    return fract(52.98291778564453*fract(dot(pixel,vec2(.0671105608344078,.005837149918079376))));
}
vec2 sample_r2(float index,float rotation) {
    return fract(vec2(.7548776865005493,.5698403120040894)*index+vec2(.5+rotation,rotation*.732));
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
vec2 original_sequence(uvec2 pixel) {
    return fract(vec2(.7548776865005493,.5698403120040894)
        *float(hilbert_index(pixel)+288u*(uint(p.size_frame.z)%64u))+.5);
}
vec3 uniform_hemisphere(vec3 n,vec2 xi) {
    // gi_trace_gi_candidates: exact uniform hemisphere + branch-safe Frisvad frame.
    float z=1.0-xi.x,phi=xi.y*6.2831854820251465;
    float r=sqrt(max(0.0,1.0-z*z));vec3 tangent,bitangent;
    if(n.z<0.0){float a=1.0/(1.0-n.z),b=n.x*n.y*a;
        bitangent=vec3(b,n.y*n.y*a-1.0,-n.y);tangent=vec3(1.0-n.x*n.x*a,-b,n.x);
    }else{float a=1.0/(1.0+n.z),b=-n.x*n.y*a;
        bitangent=vec3(b,1.0-n.y*n.y*a,-n.y);tangent=vec3(1.0-n.x*n.x*a,b,-n.x);}
    return tangent*(cos(phi)*r)+bitangent*(sin(phi)*r)+n*z;
}
float original_depth_weight(float center_z,float sample_z,vec3 view_normal){
    // Original inverse-depth ratio d_sample/d_center equals z_center/z_sample.
    return exp2(-min(30.0,80.0*max(.3,view_normal.z)*abs(1.0-center_z/sample_z)));
}
vec3 evaluate_original_sh(vec4 r,vec4 g,vec4 b,vec3 normal){
    // deferred_solid active L1 + nonlinear dominant-direction correction.
    vec3 dc=vec3(r.x,g.x,b.x),lx=vec3(r.y,g.y,b.y),ly=vec3(r.z,g.z,b.z),lz=vec3(r.w,g.w,b.w);
    vec3 tint=max(vec3(0),vec3(1)-p.indirect_tint.rgb*.92);
    vec3 dc_tinted=max(vec3(0),mix(tint/max(luminance(tint),1e-6)*luminance(dc),dc,1.17)*1.2);
    vec3 ratio=dc_tinted/max(dc,vec3(1e-6));lx*=ratio;ly*=ratio;lz*=ratio;dc=dc_tinted;
    vec3 dominant=safe_normalize(vec3(luminance(lx),luminance(ly),luminance(lz)));
    vec3 directional=abs(lx*dominant.x+ly*dominant.y+lz*dominant.z)/max(dc,vec3(1e-6));
    vec3 result=.886227548*dc+1.023327708*(normal.x*lx+normal.y*ly+normal.z*lz);
    float n=dot(dominant,normal);
    result+=dc*(directional*.08+directional*directional*.6)*.25*(.31539157*(3.0*n*n-1.0));
    return max(result,vec3(0));
}
float original_edge_weight(vec3 n0,vec3 n1,float z0,float z1) {
    // gi_blur_screen_radiance: exp2(-min(30,80*relative depth)) and
    // normal lobe .01/(1-.9*dot(n0,n1)^2)^2, with a front-facing guard.
    float nd=max(dot(n0,n1),0.0);
    float denominator=1.0-.9*nd*nd;
    return exp2(-min(30.0,80.0*abs(z1-z0)/max(abs(z0),.3)))
        *.01/(denominator*denominator)*step(.1,nd);
}
