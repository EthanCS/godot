#ifndef UV_HLSL
#define UV_HLSL
float2 get_uv(float2 pix, float4 size) { return (pix + 0.5) * size.zw; }
float2 cs_to_uv(float2 cs) { return cs * 0.5 + 0.5; }
float2 uv_to_cs(float2 uv) { return uv * 2.0 - 1.0; }
#endif
