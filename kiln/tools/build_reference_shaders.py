#!/usr/bin/env python3
"""Regenerate the checked-in GLSL ports from retained MIT Kajiya HLSL.

Requires Vulkan SDK DXC and SPIRV-Cross only when regenerating these shaders.
Normal engine builds consume the generated .comp files. Algorithms are retained;
the adapter maps engine buffers, projection convention and material encoding.
"""
from pathlib import Path
import json
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ROOT / 'servers/rendering/renderer_rd/kiln/sources'
REF = SOURCES / 'reference'
STAGES = {'kiln_rtr_temporal': 'rtr/rtr_restir_temporal.hlsl',
          'kiln_rtr_resolve': 'rtr/resolve.hlsl',
          'kiln_rtr_filter': 'rtr/temporal_filter2.hlsl',
          'kiln_rtr_cleanup': 'rtr/spatial_cleanup.hlsl',
          'kiln_restir_temporal': 'rtdgi/restir_temporal.hlsl',
          'kiln_restir_spatial': 'rtdgi/restir_spatial.hlsl',
          'kiln_restir_resolve': 'rtdgi/restir_resolve.hlsl',
          'kiln_rtdgi_validity': 'rtdgi/temporal_validity_integrate.hlsl',
          'kiln_rtdgi_temporal_filter': 'rtdgi/temporal_filter2.hlsl',
          'kiln_rtdgi_spatial_filter': 'rtdgi/spatial_filter2.hlsl',
          'kiln_ssgi': 'ssgi/ssgi.hlsl',
          'kiln_ssgi_spatial': 'ssgi/spatial_filter.hlsl',
          'kiln_ssgi_upsample': 'ssgi/upsample.hlsl',
          'kiln_ssgi_temporal': 'ssgi/temporal_filter.hlsl',
          'kiln_shadow_bitpack': 'shadow_denoise/bitpack_shadow_mask.hlsl',
          'kiln_shadow_temporal': 'shadow_denoise/megakernel.hlsl',
          'kiln_shadow_spatial': 'shadow_denoise/spatial_filter.hlsl',
          'kiln_taa_reproject': 'taa/reproject_history.hlsl',
          'kiln_taa_input': 'taa/filter_input.hlsl',
          'kiln_taa_history': 'taa/filter_history.hlsl',
          'kiln_taa_prob': 'taa/input_prob.hlsl',
          'kiln_taa_prob_filter': 'taa/filter_prob.hlsl',
          'kiln_taa_prob_filter2': 'taa/filter_prob2.hlsl',
          'kiln_taa': 'taa/taa.hlsl',
          'kiln_display_lut': 'lut/bezold_brucke.hlsl',
          'kiln_post': 'post_combine.hlsl',
          'kiln_post_blur0': 'blur.hlsl',
          'kiln_post_blur': 'blur.hlsl',
          'kiln_rtdgi_history_reproject': 'rtdgi/fullres_reproject.hlsl'}
HALF = {'kiln_rtr_temporal', 'kiln_restir_temporal', 'kiln_restir_spatial', 'kiln_rtdgi_validity', 'kiln_ssgi', 'kiln_ssgi_spatial'}


def main():
    manifest = {}
    with tempfile.TemporaryDirectory(prefix='kiln-reference-shaders-') as tmp:
        tmp = Path(tmp)
        for stage, filename in STAGES.items():
            code = (REF / filename).read_text()
            if stage.startswith('kiln_post_blur'):
                code = code.replace('Texture2D<float4> input_tex;', '[[vk::binding(0)]] Texture2D<float4> input_tex;').replace('RWTexture2D<float4> output_tex;', '[[vk::binding(1)]] RWTexture2D<float4> output_tex;')
                if stage == 'kiln_post_blur0':
                    # The first level uses rust-shaders blur.rs, whose vertical loop has ten taps.
                    code = code.replace('y <= kernel_radius * 2', 'y < kernel_radius * 2')
            if stage == 'kiln_display_lut':
                code = code.replace('RWTexture1D', 'RWTexture2D').replace('output_tex[px]', 'output_tex[uint2(px, 0)]')
            if stage == 'kiln_post':
                code = code.replace('bindless_textures[BINDLESS_LUT_BEZOLD_BRUCKE]', 'kiln_display_lut').replace('bindless_textures[BINDLESS_LUT_BLUE_NOISE_256_LDR_RGBA_0]', 'kiln_noise')
                code = '[[vk::binding(50)]] Texture2D<float4> kiln_display_lut;\n#include "' + (REF / 'inc/blue_noise.hlsl').as_posix() + '"\n' + code
            if stage in ('kiln_ssgi', 'kiln_ssgi_spatial'):
                code = re.sub(r'\bdepth_tex\b', 'half_depth_tex', code)
            if stage == 'kiln_ssgi':
                # The reference generates slice directions in Y-up clip space.
                # Godot's corrected projection and uv adapter use Y-down clip
                # space, so mirror the direction, preserving the same samples.
                code = code.replace('sin(ss_angle));', '-sin(ss_angle));')
            if stage == 'kiln_ssgi_spatial':
                code = re.sub(r'\bnormal_tex\b', 'half_view_normal_tex', code)
            code = code.replace('SamplerState sampler_lnc;', '')
            # Keep the original source intact. Drop ray tracing-only includes
            # which are not used by these compute stages.
            code = re.sub(r'#include "[^\"]*(?:atmosphere|sun|triangle|bindings)\.hlsl"', '', code)
            code = re.sub(r'#include "([^\"]+)"',
                          lambda m: '#include "' + (REF / filename).parent.joinpath(m[1]).resolve().as_posix() + '"', code)
            bindings = {}
            def binding(m):
                idx, kind, typ, name = int(m[1]) + 10, m[2], m[3], m[4]
                bindings[name] = idx
                if name in ('gbuffer_tex', 'half_view_normal_tex', 'half_depth_tex', 'geometric_normal_tex'):
                    return ''
                return f'[[vk::binding({idx})]] {kind}<{typ}> {name};'
            code = re.sub(r'\[\[vk::binding\((\d+)\)\]\] (RWTexture2D|Texture2D)<([^>]+)> (\w+);', binding, code)
            code = re.sub(r'\[\[vk::binding\(\d+\)\]\] cbuffer _ \{.*?\};', '', code, flags=re.S)
            code = re.sub(r'\bgbuffer_tex\[([^\]]+)\]', r'kiln_gbuffer_packed(\1)', code)
            code = re.sub(r'\bhalf_view_normal_tex\[([^\]]+)\]', r'kiln_half_normal(\1)', code)
            code = re.sub(r'\bhalf_depth_tex\[([^\]]+)\]', r'kiln_half_depth(\1)', code)
            code = re.sub(r'\bgeometric_normal_tex\[([^\]]+)\]', r'kiln_geometric_normal(\1)', code)
            code = code.replace('half_depth_tex.SampleLevel(sampler_nnc, cs_to_uv(interp_pos_cs.xy), 0)',
                                'kiln_half_depth(int2(cs_to_uv(interp_pos_cs.xy) * p.size_frame.xy * 0.5))')
            code = code.replace('frame_constants.view_constants.view_to_clip[1][1]', 'abs(frame_constants.view_constants.view_to_clip[1][1])')
            code = code.replace('frame_constants.view_constants.clip_to_view[1][1]', 'abs(frame_constants.view_constants.clip_to_view[1][1])')
            # The reference's zero/zero movement ratio is undefined on a static
            # camera. Choose zero motion explicitly instead of propagating NaNs.
            code = code.replace('length(reproj.xy) / length(reflector_prev_uv - uv)',
                                'length(reproj.xy) / max(1e-20, length(reflector_prev_uv - uv))')
            if stage in ('kiln_rtr_cleanup', 'kiln_rtdgi_spatial_filter'):
                # Exact axis-aligned Godot normals can have z=0. A background
                # neighbor otherwise evaluates 0 * (depth / 0), unlike the
                # quantized RGB8 normals in the reference G-buffer.
                code = code.replace('const float sample_depth = depth_tex[sample_px];',
                                    'const float sample_depth = depth_tex[sample_px];\n        if (sample_depth == 0.0) continue;')
                code = code.replace('vsum / wsum', 'vsum / max(1e-20, wsum)')
            for size_name in ('input_tex_size', 'output_tex_size', 'gbuffer_tex_size'):
                code = re.sub(r'\b' + size_name + r'\b', 'kiln_' + size_name, code)
            prefix = '#define KILN_HALF_OUTPUT\n' if stage in HALF else ''
            if stage == 'kiln_post':
                prefix += '#define ev_shift p.lighting.y\n'
            if stage.startswith('kiln_shadow_'):
                prefix += '#define bitpacked_shadow_mask_extent ((uint2(p.size_frame.xy) + uint2(7, 3)) / uint2(8, 4))\n'
            if stage == 'kiln_shadow_spatial':
                prefix += 'struct ShadowPass { uint stride; };\n[[vk::push_constant]] ConstantBuffer<ShadowPass> shadow_pass;\n#define step_size shadow_pass.stride\n'
            if stage == 'kiln_restir_spatial':
                prefix += 'struct SpatialPass { uint index; };\n[[vk::push_constant]] ConstantBuffer<SpatialPass> spatial_pass;\n#define spatial_reuse_pass_idx spatial_pass.index\n'
            code = prefix + '#include "' + (REF / 'adapter.hlsl').as_posix() + '"\n' + code
            if stage not in ('kiln_rtdgi_validity', 'kiln_display_lut', 'kiln_post_blur0', 'kiln_post_blur') and not stage.startswith('kiln_shadow_'):
                code = re.sub(r'(void main\([^)]*\)\s*\{)', r'\1\n    if (any(px >= uint2(p.size_frame.xy)' + (' / 2' if stage in HALF else '') + r')) return;', code)
            path = tmp / (stage + '.hlsl'); path.write_text(code)
            spv = tmp / (stage + '.spv')
            subprocess.run(['dxc', '-spirv', '-T', 'cs_6_0', '-E', 'main', '-fvk-use-gl-layout',
                            '-fspv-target-env=vulkan1.1', '-O3', '-Fo', str(spv), str(path)], check=True)
            # ShaderRD's set 0 uses combined image samplers.
            glsl = subprocess.check_output(['spirv-cross', str(spv), '--version', '460', '--vulkan-semantics',
                                            '--combined-samplers-inherit-bindings'], text=True)
            glsl = re.sub(r'layout\([^\n]+\) uniform sampler \w+;\n', '', glsl)
            glsl = re.sub(r'uniform sampler \w+;\n', '', glsl)
            glsl = glsl.replace('uniform texture2D ', 'uniform sampler2D ').replace('uniform utexture2D ', 'uniform usampler2D ')
            glsl = re.sub(r'[u]?sampler2D\((\w+), \w+\)', r'\1', glsl)
            samplers = {}
            def combine(m):
                idx, kind, name = m[1], m[2], m[3]
                if idx in samplers:
                    return ''
                samplers[idx] = name
                return m[0]
            declarations = list(re.finditer(r'layout\(binding = (\d+)\) uniform ([ui]?sampler2D) (\w+);', glsl))
            for m in declarations:
                if m[1] in samplers:
                    glsl = glsl.replace(m[0], '').replace(m[3], samplers[m[1]])
                else:
                    samplers[m[1]] = m[3]
            formats = {'kiln_rtr_temporal': {'hit_normal_output_tex': 'rgba16f'},
                       'kiln_rtr_resolve': {'output_tex': 'rgba16f', 'ray_len_output_tex': 'rg16f'},
                       'kiln_rtr_filter': {'output_tex': 'rgba16f'},
                       'kiln_rtr_cleanup': {'output_tex': 'rgba16f'},
                       'kiln_restir_temporal': {'irradiance_out_tex': 'rgba16f', 'hit_normal_output_tex': 'rgba16f', 'candidate_out_tex': 'rgba16f'},
                       'kiln_restir_spatial': {},
                       'kiln_restir_resolve': {'irradiance_output_tex': 'rgba16f'},
                       'kiln_rtdgi_validity': {'output_tex': 'rg16f'},
                       'kiln_rtdgi_temporal_filter': {'output_tex': 'rgba16f', 'history_output_tex': 'rgba16f', 'variance_history_output_tex': 'rg16f'},
                       'kiln_rtdgi_spatial_filter': {'output_tex': 'rgba16f'},
                       'kiln_ssgi': {'output_tex': 'r16f'},
                       'kiln_ssgi_spatial': {'output_tex': 'r16f'},
                       'kiln_ssgi_upsample': {'output_tex': 'r16f'},
                       'kiln_ssgi_temporal': {'final_output_tex': 'r8', 'history_output_tex': 'r16f'},
                       'kiln_shadow_bitpack': {},
                       'kiln_shadow_temporal': {'output_moments_tex': 'rgba16f', 'temporal_output_tex': 'rg16f'},
                       'kiln_shadow_spatial': {'output_tex': 'rg16f'},
                       'kiln_taa_reproject': {'output_tex': 'rgba16f', 'closest_velocity_output': 'rg16f'},
                       'kiln_taa_input': {'output_tex': 'rgba16f', 'dev_output_tex': 'rgba16f'},
                       'kiln_taa_history': {'output_tex': 'rgba16f'},
                       'kiln_taa_prob': {'output_tex': 'r16f'},
                       'kiln_taa_prob_filter': {'output_tex': 'r16f'},
                       'kiln_taa_prob_filter2': {'output_tex': 'r16f'},
                       'kiln_taa': {'temporal_output_tex': 'rgba16f', 'output_tex': 'rgba16f', 'smooth_var_output_tex': 'rgba16f', 'velocity_output_tex': 'rg16f'},
                       'kiln_display_lut': {'output_tex': 'rg16f'},
                       'kiln_post': {'output_tex': 'r11f_g11f_b10f'},
                       'kiln_post_blur0': {'output_tex': 'r11f_g11f_b10f'},
                       'kiln_post_blur': {'output_tex': 'r11f_g11f_b10f'},
                       'kiln_rtdgi_history_reproject': {'output_tex': 'rgba16f'}}
            for name, fmt in formats[stage].items():
                glsl = re.sub(r'(layout\((?:set = 0, )?binding = \d+, )\w+(\) uniform writeonly image2D '+name+r';)', r'\g<1>'+fmt+r'\2', glsl)
            # HLSL Texture.Load defines out-of-bounds reads as zero. GLSL's
            # texelFetch does not: clamp the actual read and select zero for
            # invalid coordinates. Boolean mix preserves zero even when the
            # clamped texel contains NaNs. Avoid a branch around every load:
            # it prevents efficient scheduling in the neighborhood filters.
            glsl = glsl.replace('texelFetch(', 'kiln_fetch(')
            fetch = '''
vec4 kiln_fetch(sampler2D t, ivec2 px, int level) {
    ivec2 extent = textureSize(t, level);
    bool valid = all(lessThan(uvec2(px), uvec2(extent)));
    return mix(vec4(0.0), texelFetch(t, clamp(px, ivec2(0), extent - 1), level), bvec4(valid));
}
uvec4 kiln_fetch(usampler2D t, ivec2 px, int level) {
    ivec2 extent = textureSize(t, level);
    bool valid = all(lessThan(uvec2(px), uvec2(extent)));
    return mix(uvec4(0), texelFetch(t, clamp(px, ivec2(0), extent - 1), level), bvec4(valid));
}
'''
            glsl = glsl.replace('void main()', fetch + '\nvoid main()')
            # These retained kernels have short, bounded neighborhood loops.
            # Preserve the opportunity to unroll after the HLSL -> GLSL trip;
            # sample counts, weights and boundary semantics stay unchanged.
            glsl = glsl.replace('#version 460', '#version 460\n#extension GL_EXT_control_flow_attributes : require')
            glsl = re.sub(r'(?m)^(\s*)for\s*\(', r'\1[[unroll]] for (', glsl)
            if stage == 'kiln_shadow_spatial':
                glsl = glsl.replace('uniform type_ConstantBuffer_ShadowPass shadow_pass;',
                                    'layout(push_constant, std430) uniform ShadowPassBlock { uint stride; } shadow_pass;')
            if stage == 'kiln_restir_spatial':
                glsl = glsl.replace('uniform type_ConstantBuffer_SpatialPass spatial_pass;',
                                    'layout(push_constant, std430) uniform SpatialPassBlock { uint index; } spatial_pass;')
            used = sorted(set(map(int, re.findall(r'layout\((?:set = 0, )?binding = (\d+)', glsl))))
            manifest[stage] = {'bindings': used, 'source_bindings': bindings}
            target = SOURCES / (stage + '.comp')
            result = '// Generated by kiln/tools/build_reference_shaders.py from retained Kajiya MIT source.\n' + glsl
            if not target.exists() or target.read_text() != result:
                target.write_text(result)
            print(stage, used)
    (REF / 'bindings.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
