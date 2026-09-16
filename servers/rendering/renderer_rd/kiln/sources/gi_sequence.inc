layout(set=0,binding=19) uniform sampler2D sample_sequence;
uint hash(uint x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
vec4 sequence_rotation(ivec2 pixel){
    uint seed=hash(uint(pixel.x)+hash(uint(pixel.y)+17u));
    return vec4(hash(seed),hash(seed+1u),hash(seed+2u),hash(seed+3u))*(1.0/4294967296.0);
}
vec4 sequence(vec4 rotation,uint index){
    // Independent primary hemisphere and bounce sky dimensions.
    // The finite sequence is computed once on this GPU, using the original
    // float32 operations; per-ray work is a single nearest texel read.
    return fract(texelFetch(sample_sequence,ivec2(int(index&127u),int(index>>7)),0)+rotation);
}
