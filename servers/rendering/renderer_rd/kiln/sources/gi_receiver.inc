layout(set=0,binding=1) uniform sampler2D scene_depth;
layout(set=0,binding=2) uniform sampler2D scene_normal;
layout(set=0,binding=11) uniform sampler2D previous_position;
layout(set=0,binding=12) uniform sampler2D previous_normal;
layout(set=0,binding=13) uniform sampler2D previous_confidence;
void receiver_context(ivec2 q,ivec2 size,out float depth,out vec3 wp,out vec3 wn,
        out vec3 stored_normal,out int rays,out uint sample_start){
    // A receiver represents one exact full-resolution texel. Sampling depth at
    // a half-grid UV but unprojecting that UV instead of the sampled texel's
    // center moves the ray origin off the surface, increasingly with distance.
    // Even full pixels form a regular grid for both odd and even viewport sizes.
    ivec2 pixel=q*int(p.engine_state.y);
    vec2 uv=(vec2(pixel)+.5)/p.size_frame.xy;
    depth=texelFetch(scene_depth,pixel,0).r;
    wp=vec3(0);wn=vec3(0);stored_normal=vec3(0);
    rays=int(p.quality.x);
    sample_start=uint(mod(p.size_frame.z,256.0)*float(rays));
    if(depth>1e-7){
        wp=world_position(uv,depth);
        wn=safe_normalize(mat3(p.inv_view)*receiver_view_normal(scene_normal,scene_depth,pixel));
        // Match the RGBA16F history representation explicitly before both
        // validation and storage: image-store conversion can round differently
        // from packHalf2x16. Keep the full-precision wn for ray integration.
        stored_normal=vec3(unpackHalf2x16(packHalf2x16(wn.xy)),unpackHalf2x16(packHalf2x16(vec2(wn.z,0))).x);
        if(p.gi.z<0.0 && rays<4){
            // Newly exposed/thin surfaces have no useful temporal estimate.
            // Spend a small local ray burst there instead of injecting a
            // one-ray outlier into the displayed light along moving edges.
            float count=0,weights=0;
            if(p.quality.y<.5){
                if(same_receiver(vec4(wp,1),stored_normal,texelFetch(previous_position,q,0),texelFetch(previous_normal,q,0).xyz)){
                    count=texelFetch(previous_confidence,q,0).r;weights=1;
                }
            }else{
                vec2 old_pixel;
                if(history_coordinates(vec4(wp,1),old_pixel)){
                    ivec2 base=ivec2(floor(old_pixel));
                    for(int y=0;y<2;y++)for(int x=0;x<2;x++){
                        ivec2 tap=base+ivec2(x,y);
                        if(any(lessThan(tap,ivec2(0)))||any(greaterThanEqual(tap,size)))continue;
                        if(!compatible_history(vec4(wp,1),stored_normal,texelFetch(previous_position,tap,0),texelFetch(previous_normal,tap,0).xyz))continue;
                        vec2 offset=abs(vec2(tap)-old_pixel);float w=(1.0-offset.x)*(1.0-offset.y);
                        count+=texelFetch(previous_confidence,tap,0).r*w;weights+=w;
                    }
                }
            }
            if(weights<1e-5 || count/max(weights,1e-5)<8.0)rays=4;
            sample_start=uint(mod(p.size_frame.z,256.0)*float(rays));
        }
        // Exact finite-batch caching requires an unjittered receiver. With TAA,
        // trace fresh frame-sequenced samples and accumulate reprojected history.
        if(p.gi.z>=0.0 && p.quality.y<.5){
            bool valid=stationary_history(vec4(wp,1),stored_normal,texelFetch(previous_position,q,0),texelFetch(previous_normal,q,0).xyz);
            float count=valid?texelFetch(previous_confidence,q,0).r:0.0;
            sample_start=uint(count);
            rays=max(0,min(rays,int(p.gi.w-count)));

        }
    }else rays=0;
}
