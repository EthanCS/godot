#ifndef BLUE_NOISE_HLSL
#define BLUE_NOISE_HLSL
#include "quasi_random.hlsl"
[[vk::binding(3)]] Texture2D<float4> kiln_noise;
float4 blue_noise_for_pixel(uint2 px, uint n) {
    uint2 offset = r2_sequence(n) * 256;
    return kiln_noise[(px + offset) & 255] * 255.0 / 256.0 + 0.5 / 256.0;
}
#endif
