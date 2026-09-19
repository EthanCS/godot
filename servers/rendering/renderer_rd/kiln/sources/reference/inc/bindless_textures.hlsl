#ifndef KILN_BINDLESS
#define KILN_BINDLESS
#include "samplers.hlsl"
[[vk::binding(8)]] Texture2D<float4> kiln_fg;
#define BINDLESS_LUT_BRDF_FG 0
#define BINDLESS_LUT_BLUE_NOISE_256_LDR_RGBA_0 1
// Call sites selecting BRDF FG use this one-element alias.

#endif
