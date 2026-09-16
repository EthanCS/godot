// TAA reprojects between history texels. Normalize geometrically compatible
// bilinear taps instead of repeatedly losing edge history to one nearest tap.
bool compatible_history(vec4 current,vec3 normal,vec4 old,vec3 old_normal){
    float threshold=max(p.quality.w,length(current.xyz-p.inv_view[3].xyz)*.002);
    return old.w>.5 && dot(normal,old_normal)>.85
        && distance(old.xyz,current.xyz)<threshold
        && abs(dot(old.xyz-current.xyz,normal))<threshold*.35;
}
bool history_coordinates(vec4 position,out vec2 pixel){
    vec4 clip=p.previous_view_projection*vec4(position.xyz,1);
    vec2 uv=clip.xy/max(clip.w,1e-7)*.5+.5;
    // Integrate samples full texel centers 2*q+.5, including odd dimensions.
    pixel=(uv*p.size_frame.xy-.5)*.5;
    return p.size_frame.w>.5 && position.w>.5 && clip.w>0.0
        && all(greaterThanEqual(uv,vec2(0))) && all(lessThan(uv,vec2(1)));
}
// With an unjittered stationary camera, only reuse the same receiver within
// float32 reconstruction precision, not a moving-reprojection neighbourhood.
// Camera stability alone says nothing about animated meshes or disocclusion.
bool same_receiver(vec4 current,vec3 normal,vec4 old,vec3 old_normal){
    // Resolved depth can vary by a few rounding units even at an unchanged
    // camera pose. Bit equality resets real static surfaces every few frames.
    // Include camera magnitude to cover cancellation during unprojection.
    vec3 tolerance=max(vec3(1),max(abs(current.xyz),abs(p.inv_view[3].xyz)))*(8.0*1.1920929e-7);
    return p.size_frame.w>.5 && current.w>.5 && old.w>.5
        && all(lessThanEqual(abs(current.xyz-old.xyz),tolerance))
        // Integrate explicitly rounds its receiver normal to RGBA16F before
        // validation AND storage, so both passes decide on identical values.
        && all(equal(normal,old_normal));
}
bool stationary_history(vec4 current,vec3 normal,vec4 old,vec3 old_normal){
    return p.gi.z>0.0 && same_receiver(current,normal,old,old_normal);
}
