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
layout(set=0,binding=18,std430) readonly buffer Emitters { vec4 emitters[]; };
struct TextureCoordinates { vec4 ab; vec4 c_page_repeat; };
layout(set=0,binding=32) uniform sampler2DArray ray_albedo;
layout(set=0,binding=33,std430) readonly buffer StaticUVs { TextureCoordinates static_uvs[]; };
layout(set=0,binding=34,std430) readonly buffer DynamicUVs { TextureCoordinates dynamic_uvs[]; };
Triangle world_triangle(int id){return id<int(p.source_bvh_state.y)?triangles[id]:dynamic_triangles[id-int(p.source_bvh_state.y)];}
vec3 hit_albedo(int id, vec3 position) {
    Triangle t = world_triangle(id);
    TextureCoordinates record = id < int(p.source_bvh_state.y) ? static_uvs[id] : dynamic_uvs[id-int(p.source_bvh_state.y)];
    if (record.c_page_repeat.z < 0.5) return t.albedo.rgb;
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
    return t.albedo.rgb*color;
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
            float t=dot(tri.p2.xyz,q)*inv;if(t<=.006||t>=maximum)continue;
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
            vec3 h=cross(ray,e2);float det=dot(e1,h);if(abs(det)<1e-9)continue;
            float inv=1.0/det;vec3 s=origin-tri.p0.xyz;float u=dot(s,h)*inv;
            if(u<0.0||u>1.0)continue;
            vec3 q=cross(s,e1);float v=dot(ray,q)*inv;if(v<0.0||u+v>1.0)continue;
            float t=dot(e2,q)*inv;if(t<=.006||t>=distance)continue;
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
bool world_trace_hardware(vec3 origin,vec3 ray,float maximum,bool any_hit,out int triangle_id,out float distance,out vec3 normal){
    triangle_id=-1;distance=maximum;normal=vec3(0);
    rayQueryEXT query;
    uint flags=gl_RayFlagsOpaqueEXT|(any_hit?gl_RayFlagsTerminateOnFirstHitEXT:0u);
    rayQueryInitializeEXT(query,scene_tlas,flags,255u,origin,.006,ray,maximum);
    while(rayQueryProceedEXT(query)){}
    if(rayQueryGetIntersectionTypeEXT(query,true)==gl_RayQueryCommittedIntersectionNoneEXT)return false;
    int instance=int(rayQueryGetIntersectionInstanceCustomIndexEXT(query,true));
    triangle_id=int(rayQueryGetIntersectionPrimitiveIndexEXT(query,true))+(instance==1?int(p.source_bvh_state.y):0);
    distance=rayQueryGetIntersectionTEXT(query,true);
    Triangle tri=world_triangle(triangle_id);
    normal=safe_normalize(vec3(tri.p0.w,tri.p1.w,tri.p2.w));
    if(dot(normal,ray)>0.0)normal=-normal;
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
    int id;float distance;vec3 normal;
    return world_trace(origin,direction,maximum,true,id,distance,normal);
}
bool occluded(vec3 origin,vec3 direction){
    return visibility_blocked(origin,direction,1000.0);
}
vec3 sky_at_surface(vec3 position,vec3 normal,vec2 xi){
    vec3 ray=cosine_direction(normal,xi);
    if(p.sun_color.w<=0.0||ray.y<=0.0)return vec3(0);
    return occluded(position+normal*.025,ray)?vec3(0):environment_radiance(ray);
}
// Area-weighted next-event sampling makes a small crystal useful at the
// production ray budget. This is surface emission with world-space visibility,
// not an unshadowed point light or a screen-space bloom approximation.
vec3 emitter_sample(vec3 position,vec3 normal,vec3 xi,out vec3 direction){
    direction=normal;
    int count=int(p.dynamic_scene.z);if(count==0)return vec3(0);
    float target=xi.x*p.dynamic_scene.w;int lo=0,hi=count-1;
    while(lo<hi){int mid=(lo+hi)/2;if(emitters[mid].y<target)lo=mid+1;else hi=mid;}
    vec4 entry=emitters[lo];Triangle tri=world_triangle(int(entry.x));
    float u=sqrt(xi.y);vec3 point=tri.p0.xyz+tri.p1.xyz*(u*(1.0-xi.z))+tri.p2.xyz*(u*xi.z);
    vec3 origin=position+normal*.012,delta=point-origin;float squared=dot(delta,delta);
    if(squared<.0004)return vec3(0);
    float distance=sqrt(squared);direction=delta/distance;
    if(dot(normal,direction)<=0.0)return vec3(0);
    vec3 source_normal=safe_normalize(cross(tri.p1.xyz,tri.p2.xyz));
    float cosine=abs(dot(source_normal,-direction));
    if(cosine<.0001)return vec3(0);
    int id;float hit_distance;vec3 n;
    if(visibility_blocked(origin,direction,distance-.008))return vec3(0);
    return tri.emission.rgb*(entry.z*cosine*p.dynamic_scene.w/(squared*entry.w));
}
// World-space light grid: each cell stores a complete, dynamically sized list.
// One uniformly sampled overlapping analytic light per bounce is unbiased; the
// sample weight includes list length. Visibility uses the same software BVH.
layout(set=0,binding=25,std430) readonly buffer LocalLights { vec4 local_lights[]; };
layout(set=0,binding=26,std430) readonly buffer LightGrid { uint light_grid[]; };
vec3 local_light_unoccluded(uint id,vec3 position,vec3 normal,out vec3 direction,out float distance){
    vec4 pr=local_lights[2u+id*4u],color=local_lights[3u+id*4u],cone=local_lights[4u+id*4u];
    vec3 delta=pr.xyz-position;distance=length(delta);direction=delta/max(distance,.0001);
    if(distance>=pr.w||distance<.001)return vec3(0);
    float nl=max(0.0,dot(normal,direction));if(nl<=0.0)return vec3(0);
    float window=max(1.0-pow(distance/pr.w,4.0),0.0);
    float attenuation=window*window*pow(max(distance,.0001),-color.w);
    if(cone.w>=-1.0){
        float scos=max(dot(-direction,cone.xyz),cone.w);
        float rim=max(1e-4,(1.0-scos)/(1.0-cone.w));
        attenuation*=1.0-pow(rim,local_lights[5u+id*4u].x);
    }
    return max(vec3(0),color.rgb*(attenuation*nl/PI));
}
vec3 local_light_sample(vec3 position,vec3 normal,float xi){
    vec4 origin=local_lights[0];ivec3 dims=ivec3(local_lights[1].xyz);
    ivec3 cell=ivec3(floor((position-origin.xyz)/origin.w));
    if(any(lessThan(cell,ivec3(0)))||any(greaterThanEqual(cell,dims)))return vec3(0);
    int cell_id=(cell.z*dims.y+cell.y)*dims.x+cell.x;
    uint count=light_grid[cell_id*2+1],start=light_grid[cell_id*2];
    // Importance-sample actual incident power, including range and spot cone.
    // A uniform light index spent most samples on irrelevant lamps in Sponza.
    // The CDF uses cheap unoccluded weights, then traces ONE visibility ray.
    float total=0.0,distance;vec3 direction;
    for(uint i=0u;i<count;i++)total+=luminance(local_light_unoccluded(light_grid[start+i],position,normal,direction,distance));
    if(total<=1e-10)return vec3(0);
    float target=xi*total,accum=0.0;
    for(uint i=0u;i<count;i++){
        vec3 value=local_light_unoccluded(light_grid[start+i],position,normal,direction,distance);
        float weight=luminance(value);accum+=weight;
        if(weight>0.0 && (accum>=target || i+1u==count)){
            if(visibility_blocked(position+normal*.025,direction,distance-.03))return vec3(0);
            return value*(total/weight);
        }
    }
    return vec3(0);
}
