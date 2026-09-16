// A changed half-resolution receiver affects at most 2+4+8=14 pixels
// through the three a-trous passes. One adjacent 16-pixel tile is conservative.
layout(set=0,binding=GI_DIRTY_BINDING,std430) readonly buffer DirtyTiles { uint dirty_tiles[]; };
bool filter_region_dirty(ivec2 pixel){
    ivec2 size=(ivec2(p.debug.yz)+15)/16,tile=pixel/16;
    for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++){
        ivec2 q=tile+ivec2(x,y);
        if(any(lessThan(q,ivec2(0)))||any(greaterThanEqual(q,size)))continue;
        if(dirty_tiles[q.y*size.x+q.x]!=0u)return true;
    }
    return false;
}
