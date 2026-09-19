#ifndef KILN_TRACE_MIN_DISTANCE
#define KILN_TRACE_MIN_DISTANCE 0.006
#endif
#ifndef KILN_VISIBILITY_MIN_DISTANCE
#define KILN_VISIBILITY_MIN_DISTANCE KILN_TRACE_MIN_DISTANCE
#endif
// Threaded SAH BVH. Internal: lo.w=-1, hi.w=escape node after subtree.
// Leaf: lo.w=first triangle, hi.w=-triangle count-1. Children follow their parent.
// Escape links identify the right child for near-first traversal; depth < 32 is
// validated when building/loading the hierarchy. Triangle p1/p2 are cached edges.
struct BvhNode { vec4 lo; vec4 hi; };
struct Triangle { vec4 p0; vec4 p1; vec4 p2; vec4 albedo; vec4 emission; };
layout(set=0,binding=4,std430) readonly buffer Nodes { BvhNode nodes[]; };
layout(set=0,binding=5,std430) readonly buffer Triangles { Triangle triangles[]; };
layout(set=0,binding=16,std430) readonly buffer DynamicNodes { BvhNode dynamic_nodes[]; };
layout(set=0,binding=17,std430) readonly buffer DynamicTriangles { Triangle dynamic_triangles[]; };
struct TextureCoordinates { vec4 ab; vec4 c_page_repeat; vec4 base_roughness; };
layout(set=0,binding=32) uniform sampler2DArray ray_albedo;
layout(set=0,binding=33,std430) readonly buffer StaticUVs { TextureCoordinates static_uvs[]; };
layout(set=0,binding=34,std430) readonly buffer DynamicUVs { TextureCoordinates dynamic_uvs[]; };
Triangle world_triangle(int id){return id<int(p.source_bvh_state.y)?triangles[id]:dynamic_triangles[id-int(p.source_bvh_state.y)];}
vec4 hit_base_roughness(int id, vec3 position) {
    Triangle t = world_triangle(id);
    TextureCoordinates record = id < int(p.source_bvh_state.y) ? static_uvs[id] : dynamic_uvs[id-int(p.source_bvh_state.y)];
    if (record.c_page_repeat.z < 0.5) return record.base_roughness;
    vec3 delta = position - t.p0.xyz;
    float aa = dot(t.p1.xyz,t.p1.xyz), ab = dot(t.p1.xyz,t.p2.xyz), bb = dot(t.p2.xyz,t.p2.xyz);
    float da = dot(delta,t.p1.xyz), db = dot(delta,t.p2.xyz), denominator = max(aa*bb-ab*ab,1e-20);
    vec2 bary = vec2(da*bb-db*ab, db*aa-da*ab)/denominator;
    vec2 uv = record.ab.xy*(1.0-bary.x-bary.y)+record.ab.zw*bary.x+record.c_page_repeat.xy*bary.y;
    // The sampler repeats; clamping to texel centers gives clamp-to-edge pages
    // without a second sampler or an extra fetch. sRGB is decoded by hardware
    // before filtering, matching the original four-tap linear-light estimator.
    uv = record.c_page_repeat.w > 0.5 ? fract(uv) : clamp(uv, vec2(0.5/512.0), vec2(511.5/512.0));
    vec3 color = textureLod(ray_albedo, vec3(uv, record.c_page_repeat.z - 1.0), 0.0).rgb;
    return vec4(record.base_roughness.rgb * color, record.base_roughness.w);
}
BvhNode world_node(int id,bool dynamic){return dynamic?dynamic_nodes[id]:nodes[id];}
float box_near(vec3 origin,vec3 inverse_ray,BvhNode node,float maximum){
    vec3 a=(node.lo.xyz-origin)*inverse_ray,b=(node.hi.xyz-origin)*inverse_ray;
    vec3 lo=min(a,b),hi=max(a,b);
    float near_distance=max(0.0,max(lo.x,max(lo.y,lo.z)));
    return near_distance<=min(maximum,min(hi.x,min(hi.y,hi.z)))?near_distance:-1.0;
}
// Visibility rays need only the first blocker. Escape links let them walk
// without a per-ray stack or sorting/loading both children at every branch.
bool trace_blocker(vec3 origin,vec3 ray,float maximum,bool dynamic,out int triangle_id,out float distance){
    triangle_id=-1;distance=maximum;
    int node_count=int(dynamic?p.dynamic_scene.x:p.source_bvh_state.x);
    vec3 inverse_ray=1.0/(mix(vec3(1e-8),vec3(-1e-8),lessThan(ray,vec3(0)))+ray);
    int index=0;
    while(index<node_count){
        BvhNode node=world_node(index,dynamic);
        bool leaf=node.hi.w<0.0;
        if(box_near(origin,inverse_ray,node,maximum)<0.0){index=leaf?index+1:int(node.hi.w);continue;}
        if(!leaf){index++;continue;}
        int first=int(node.lo.w),end=first-int(node.hi.w)-1;
        for(int i=first;i<end;i++){
            Triangle tri=dynamic?dynamic_triangles[i]:triangles[i];
            vec3 h=cross(ray,tri.p2.xyz);float det=dot(tri.p1.xyz,h);if(abs(det)<1e-9)continue;
            float inv=1.0/det;vec3 s=origin-tri.p0.xyz;float u=dot(s,h)*inv;
            if(u<0.0||u>1.0)continue;
            vec3 q=cross(s,tri.p1.xyz);float v=dot(ray,q)*inv;if(v<0.0||u+v>1.0)continue;
            float t=dot(tri.p2.xyz,q)*inv;if(t<=KILN_VISIBILITY_MIN_DISTANCE||t>=maximum)continue;
            distance=t;triangle_id=i+(dynamic?int(p.source_bvh_state.y):0);return true;
        }
        index++;
    }
    return false;
}
#ifndef GI_VISIBILITY_ONLY
bool trace_hierarchy(vec3 origin,vec3 ray,float maximum,bool any_hit,bool dynamic,out int triangle_id,out float distance){
    if(any_hit)return trace_blocker(origin,ray,maximum,dynamic,triangle_id,distance);
    triangle_id=-1;distance=maximum;
    vec3 inverse_ray=1.0/(mix(vec3(1e-8),vec3(-1e-8),lessThan(ray,vec3(0)))+ray);
    // Visit the near child first so a nearby hit prunes the far subtree.
    // Fixed preorder walked distant geometry before finding the nearest hit,
    // allowing a few grazing rays to monopolize an entire GPU workgroup.
    int pending[32];int count=0,index=(dynamic?p.dynamic_scene.x:p.source_bvh_state.x)>0.0?0:-1;
    while(index>=0){
        BvhNode node=world_node(index,dynamic);
        bool leaf=node.hi.w<0.0;
        if(box_near(origin,inverse_ray,node,distance)<0.0){
            index=count>0?pending[--count]:-1;continue;
        }
        if(!leaf){
            int left=index+1;
            BvhNode left_node=world_node(left,dynamic);
            int right=left_node.hi.w<0.0?left+1:int(left_node.hi.w);
            float a=box_near(origin,inverse_ray,left_node,distance);
            float b=box_near(origin,inverse_ray,world_node(right,dynamic),distance);
            if(a>=0.0&&b>=0.0){pending[count++]=a<=b?right:left;index=a<=b?left:right;}
            else if(a>=0.0)index=left;
            else if(b>=0.0)index=right;
            else index=count>0?pending[--count]:-1;
            continue;
        }
        int first=int(node.lo.w),end=first-int(node.hi.w)-1;
        for(int i=first;i<end;i++){
            Triangle tri=dynamic?dynamic_triangles[i]:triangles[i];vec3 e1=tri.p1.xyz,e2=tri.p2.xyz;
            vec3 h=cross(ray,e2);float det=dot(e1,h);
#ifdef KILN_TRACE_CULL_BACK_FACES
            // Godot stores clockwise front faces (opposite the glTF input).
            if(det >= -1e-9)continue;
#else
            if(abs(det)<1e-9)continue;
#endif
            float inv=1.0/det;vec3 s=origin-tri.p0.xyz;float u=dot(s,h)*inv;
            if(u<0.0||u>1.0)continue;
            vec3 q=cross(s,e1);float v=dot(ray,q)*inv;if(v<0.0||u+v>1.0)continue;
            float t=dot(e2,q)*inv;if(t<=KILN_TRACE_MIN_DISTANCE||t>=distance)continue;
            distance=t;triangle_id=i+(dynamic?int(p.source_bvh_state.y):0);
            if(any_hit)return true;
        }
        index=count>0?pending[--count]:-1;
    }
    return triangle_id>=0;
}
#endif
bool world_trace_software(vec3 origin,vec3 ray,float maximum,bool any_hit,out int triangle_id,out float distance,out vec3 normal){
    normal=vec3(0);
#ifdef GI_VISIBILITY_ONLY
    // The lighting stage issues only visibility segments. Keep the nearest-hit
    // stack out of this kernel entirely, even on compilers without call-site
    // specialization of the any_hit argument.
    if(trace_blocker(origin,ray,maximum,false,triangle_id,distance))return true;
    return trace_blocker(origin,ray,maximum,true,triangle_id,distance);
#else
    bool hit=trace_hierarchy(origin,ray,maximum,any_hit,false,triangle_id,distance);
    if(hit&&any_hit)return true;
    int dynamic_id;float dynamic_distance;
    if(trace_hierarchy(origin,ray,distance,any_hit,true,dynamic_id,dynamic_distance)){
        triangle_id=dynamic_id;distance=dynamic_distance;hit=true;
    }
    if(hit){
        Triangle tri=world_triangle(triangle_id);
        normal=safe_normalize(vec3(tri.p0.w,tri.p1.w,tri.p2.w));
        if(dot(normal,ray)>0.0)normal=-normal;
    }
    return hit;
#endif
}
#ifdef KILN_HARDWARE_RAY_QUERY
layout(set=0,binding=27) uniform accelerationStructureEXT scene_tlas;
bool world_trace_hardware(vec3 origin,vec3 direction,float maximum,bool any_hit,out int triangle_id,out float distance,out vec3 normal){
    triangle_id=-1;distance=maximum;normal=vec3(0);
    rayQueryEXT query;
    // Keep both flag alternatives constant so the Metal translator can prove
    // that traversal is opaque and lower it to its native intersector.
    #ifdef KILN_TRACE_CULL_BACK_FACES
    rayQueryInitializeEXT(query,scene_tlas,any_hit?(gl_RayFlagsOpaqueEXT|gl_RayFlagsTerminateOnFirstHitEXT):(gl_RayFlagsOpaqueEXT|gl_RayFlagsCullFrontFacingTrianglesEXT),255u,origin,KILN_TRACE_MIN_DISTANCE,direction,maximum);
#else
    rayQueryInitializeEXT(query,scene_tlas,any_hit?(gl_RayFlagsOpaqueEXT|gl_RayFlagsTerminateOnFirstHitEXT):gl_RayFlagsOpaqueEXT,255u,origin,KILN_TRACE_MIN_DISTANCE,direction,maximum);
#endif
    while(rayQueryProceedEXT(query)){}
    if(rayQueryGetIntersectionTypeEXT(query,true)==gl_RayQueryCommittedIntersectionNoneEXT)return false;
    int instance=int(rayQueryGetIntersectionInstanceCustomIndexEXT(query,true));
    triangle_id=int(rayQueryGetIntersectionPrimitiveIndexEXT(query,true))+(instance==1?int(p.source_bvh_state.y):0);
    distance=rayQueryGetIntersectionTEXT(query,true);
    Triangle tri=world_triangle(triangle_id);
    normal=safe_normalize(vec3(tri.p0.w,tri.p1.w,tri.p2.w));
    if(dot(normal,direction)>0.0)normal=-normal;
    return true;
}
#endif
bool world_trace(vec3 origin,vec3 ray,float maximum,bool any_hit,out int triangle_id,out float distance,out vec3 normal){
#ifdef KILN_HARDWARE_RAY_QUERY
    return world_trace_hardware(origin,ray,maximum,any_hit,triangle_id,distance,normal);
#else
    return world_trace_software(origin,ray,maximum,any_hit,triangle_id,distance,normal);
#endif
}
bool visibility_blocked(vec3 origin,vec3 direction,float maximum){
#ifdef KILN_HARDWARE_RAY_QUERY
    // A visibility segment needs only the hit bit. Keep this query separate
    // from closest-hit shading so primitive/material/normal payloads never
    // enter its native Metal intersector or the surrounding live state.
    rayQueryEXT query;
    rayQueryInitializeEXT(query,scene_tlas,gl_RayFlagsOpaqueEXT|gl_RayFlagsTerminateOnFirstHitEXT,255u,origin,KILN_VISIBILITY_MIN_DISTANCE,direction,maximum);
    while(rayQueryProceedEXT(query)){}
    return rayQueryGetIntersectionTypeEXT(query,true)!=gl_RayQueryCommittedIntersectionNoneEXT;
#else
    int id;float distance;vec3 normal;
    return world_trace(origin,direction,maximum,true,id,distance,normal);
#endif
}
bool occluded(vec3 origin,vec3 direction){
    return visibility_blocked(origin,direction,1000.0);
}
