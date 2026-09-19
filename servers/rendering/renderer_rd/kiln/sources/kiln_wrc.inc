// Kajiya wrc/{lookup,wrc_settings}.hlsl, MIT.
const vec3 KILN_WRC_DIMS = vec3(8,3,8);
const vec3 KILN_WRC_CENTER = vec3(0,0,8);
const float KILN_WRC_MIN_DISTANCE = 1.2599210498948732;
vec3 kiln_octa_decode(vec2 f) {
    f = f*2.0-1.0;
    vec3 n=vec3(f,1.0-abs(f.x)-abs(f.y));
    n.xy -= (step(vec2(0),n.xy)*2.0-1.0)*clamp(-n.z,0.0,1.0);
    return normalize(n);
}
vec2 kiln_octa_encode(vec3 n) {
    n/=abs(n.x)+abs(n.y)+abs(n.z);
    if(n.z<0.0)n.xy=(1.0-abs(n.yx))*mix(vec2(-1),vec2(1),greaterThanEqual(n.xy,vec2(0)));
    return n.xy*0.5+0.5;
}
struct KilnFarField { vec3 radiance; float probe_t; float surface_t; float inv_pdf; };
#ifdef KILN_WRC_LOOKUP
layout(set=0,binding=50) uniform sampler2D wrc_atlas;
KilnFarField kiln_wrc_query(vec3 origin,vec3 direction,vec3 normal) {
    KilnFarField r=KilnFarField(vec3(0),-1.0,-1.0,1.0);
    vec3 a=(KILN_WRC_CENTER+KILN_WRC_DIMS*0.5-origin)/direction;
    vec3 b=(KILN_WRC_CENTER-KILN_WRC_DIMS*0.5-origin)/direction;
    vec3 lo=min(a,b),hi=max(a,b);
    float tmin=max(lo.x,max(lo.y,lo.z)),tmax=min(hi.x,min(hi.y,hi.z));
    if(!(tmin<=tmax && tmax>=0.0))return r;
    vec3 in_box=origin+direction*max(0.0,tmin);
    ivec3 coord=clamp(ivec3(floor(in_box-KILN_WRC_CENTER+KILN_WRC_DIMS*0.5)),ivec3(0),ivec3(7,2,7));
    vec3 center=vec3(coord)-KILN_WRC_DIMS*0.5+0.5+KILN_WRC_CENTER;
    vec3 oc=origin-center;
    float qa=dot(direction,direction),qb=2.0*dot(oc,direction),qc=dot(oc,oc)-KILN_WRC_MIN_DISTANCE*KILN_WRC_MIN_DISTANCE;
    float h=qb*qb-4.0*qa*qc;
    if(h<0.0)return r;
    float t=(-qb+sqrt(h))/(2.0*qa);
    if(t<0.0)return r;
    vec3 offset=origin+direction*t-center;
    int index=coord.x+coord.y*8+coord.z*24;
    vec2 tile=vec2(index%16,index/16);
    vec2 uv=(tile+clamp(kiln_octa_encode(normalize(offset)),vec2(0.5/32.0),vec2(1.0-0.5/32.0)))/16.0;
    vec4 value=textureLod(wrc_atlas,uv,0.0);
    if(all(notEqual(normal,vec3(0)))) {
        float distance_to_box=length(in_box-origin);
        float jacobian=dot(direction*t,direction*t)/pow(distance_to_box+KILN_WRC_MIN_DISTANCE,2.0)/dot(direction,normalize(offset));
        jacobian*=dot(normal,normalize(offset))/dot(normal,direction);
        r.inv_pdf=max(0.0,jacobian);
    }
    r.radiance=value.rgb;r.probe_t=t+0.5;r.surface_t=-1.0+t+value.a-KILN_WRC_MIN_DISTANCE;
    return r;
}
#endif
