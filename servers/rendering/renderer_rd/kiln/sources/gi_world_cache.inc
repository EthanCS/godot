// World-space incident-light samples. The table contains independent integrals,
// never spatially filtered/display history. Readers run before the writer pass.
struct WorldLight { ivec4 key; vec4 position_count; vec4 normal_visibility; vec4 r; vec4 g; vec4 b; };
#ifdef GI_WORLD_LIGHT_WRITE
layout(set=0,binding=22,std430) buffer WorldLights { WorldLight world_lights[]; };
#else
layout(set=0,binding=22,std430) readonly buffer WorldLights { WorldLight world_lights[]; };
#endif
#ifdef GI_WORLD_OWNER_WRITE
layout(set=0,binding=23,std430) buffer WorldOwners { uint world_owners[]; };
#else
layout(set=0,binding=23,std430) readonly buffer WorldOwners { uint world_owners[]; };
#endif
ivec4 world_key(vec3 position,vec3 normal){
    vec3 an=abs(normal);
    int axis=an.x>an.y?(an.x>an.z?0:2):(an.y>an.z?1:2);
    return ivec4(ivec3(floor(position/0.08)),int(p.voxel_state.w)*8+axis*2+int(normal[axis]<0.0));
}
uint world_slot(ivec4 key){
    uvec3 h=uvec3(key.xyz)*uvec3(73856093u,19349663u,83492791u);
    uint v=h.x^h.y^h.z^uint(key.w&7)*2654435761u;
    v^=v>>16;v*=2246822519u;v^=v>>13;
    return v&(uint(p.voxel_size.w)-1u);
}
bool world_match(WorldLight entry,ivec4 key,vec3 position,vec3 normal){
    vec3 delta=position-entry.position_count.xyz;
    return all(equal(entry.key,key)) && entry.position_count.w>=32.0
        && dot(entry.normal_visibility.xyz,normal)>.999
        && dot(delta,delta)<.08*.08 && abs(dot(delta,normal))<.008;
}
bool world_lookup(vec3 position,vec3 normal,out uint slot){
    ivec4 key=world_key(position,normal);
    ivec3 base=ivec3(floor(position/.08-.5));
    float nearest=.08*.08;bool found=false;slot=0u;
    for(int z=0;z<2;z++)for(int y=0;y<2;y++)for(int x=0;x<2;x++){
        ivec4 candidate=ivec4(base+ivec3(x,y,z),key.w);uint address=world_slot(candidate);
        WorldLight entry=world_lights[address];
        if(!world_match(entry,candidate,position,normal))continue;
        vec3 delta=position-entry.position_count.xyz;float squared=dot(delta,delta);
        if(squared<nearest){nearest=squared;slot=address;found=true;}
    }
    return found;
}
