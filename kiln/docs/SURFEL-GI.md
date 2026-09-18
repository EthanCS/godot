# Kiln G-buffer and Surfel GI

The active renderer is standard deferred rendering. The visibility-buffer pass,
primitive/subinstance IDs and geometry material replay were removed at the user's
request. Forward+, Mobile and Compatibility remain separately selectable upstream
renderers. The implementation is engine-level RenderingDevice work, not a
compositor overlay.

## Implemented

1. Opaque/cutout materials rasterize directly into a six-attachment G-buffer:
   albedo/metallic, shading normal/roughness, emission, material response,
   instance/geometric-normal metadata, and motion. Reversed-Z depth selects the
   nearest surface. Native clustered fullscreen lighting, upstream shadows,
   forward transparency/refraction, sky and HDR post-processing remain in order.
   The 8-byte surface metadata is not a visibility-buffer material resolver.
2. A viewport-scaled pool of 65,536–262,144 persistent surfels follows original
   triangle anchors. A three-level hashed spatial grid uses compact overlap
   lists, prefix sums and scatter; world-space claims deduplicate simultaneous
   allocations from screen tiles. Queries retain every compatible contributor.
   MSME integrates traced diffuse
   irradiance; previous-cache feedback provides diffuse multibounce. Allocation,
   trace and integration dispatches are separate to avoid concurrent feedback.
3. World-space GGX visible-normal reflection rays use the primary shading normal,
   view and roughness. Hits evaluate emission, shadowed sun/local light and the
   diffuse surfel cache. Missing cache support traces sky/emitter illumination;
   misses sample the shared sky. This sees geometry outside the camera frustum.
   Two Schlick RGB integrals preserve colored Fresnel, metallic response and the
   engine's multiple-scattering compensation. No final-screen color is traced or
   copied into the reflection output.
4. Reflection filtering is separate from diffuse history. It checks receiver
   position, shading normal, roughness and hit distance, clips history using
   neighboring samples, and shortens/invalidates history during camera, geometry
   or lighting changes. Rough surfaces share more spatial samples. Screen AO
   modulates diffuse; ray-traced specular already contains geometric visibility.
   Rough receivers (roughness >= 0.45) use alternating half-rate samples when
   paired geometry agrees, reconstructed with the same filter. Sharp/wet
   receivers and discontinuities retain full-rate rays. Disable
   `rendering/kiln/specular_checkerboard` for full-rate reference sampling.
5. Original rigid scene meshes feed Vulkan/Metal hardware ray queries or compute BVH
   fallback. Opaque BaseMaterial3D albedo textures are sampled at barycentric UV1
   coordinates from 512x512 RGBA8 sRGB array layers, with hardware decoding and
   bilinear filtering, repeat/clamp, UV scale and offset. Material/texture changes update
   transport without rebuilding acceleration structures. Freed texture pages are
   reused. Screen resources are recreated on viewport reconfiguration; world-space
   surfel history and acceleration structures survive resize. Cache growth and
   relighting results are recorded in the
   [dynamic diffuse review](DIFFUSE-RELIGHTING-2026-09-18.md).
6. Forward+ consumes the same diffuse and two specular integrals. Its packed
   depth-prepass roughness is decoded before tracing. The existing direct BRDF,
   transparency and other renderers remain independent.
7. Sponza duplicates the original floor material at runtime: roughness 0.18,
   dielectric specular 0.5, metallic 0, original albedo texture. `--dry-floor`
   selects 0.85, `--roughness=<value>` overrides it, `--no-specular` disables only
   indirect specular, and debug view 28 isolates it. Deterministic suites include
   F0/metallic response, roughness spread, offscreen reflectors and motion.

The reflection budget is `rendering/kiln/specular_rays` (1–8, default 2), independent
of diffuse surfel quality. `rendering/kiln/surfel_specular` controls reflection;
`rendering/kiln/surfel_multibounce` controls diffuse-cache feedback.

Metal backend implementation, platform gates and separate build/GPU validation
are recorded in [METAL-RAY-QUERY.md](METAL-RAY-QUERY.md).

## Reference and differences

Diffuse cache/MSME reference: [SurfelPlus](https://github.com/WANG-Ruipeng/SurfelPlus),
commit `33cdd8bb7cea486c4ef53d1c00bc04e5f9a239bf`, by Zhen Ren, Ruipeng Wang and
Jinxiang Wang. Kiln retains only the MSME implementation it actually compiles,
now in `servers/rendering/renderer_rd/kiln/sources/msme.inc`. The
SurfelPlus Apache-2.0 license, NOTICE and NVIDIA MSME MIT notice are colocated in
`servers/rendering/renderer_rd/kiln/licenses/`. Historical validation manifests
still record the original reference snapshot and hashes; that unused snapshot is
no longer part of the tree.

The renderer deliberately differs from that sample: standard G-buffer material
rasterization, a sparse grid and triangle anchors, and explicit GGX reflection
rays coupled to diffuse surfels. It does not implement SurfelPlus's directional
6x6 radiance/depth atlas, ray guiding, stochastic neighbor sharing or RIS glossy
pipeline. No feature-parity or performance claim is made. GGX sampling follows
[Heitz, JCGT 7(4), 2018](https://jcgt.org/published/0007/04/01/), implemented directly
in the native compute pass rather than copied from the reference renderer.

## Compiled

Windows x86_64 Mono editor and release export template, MSVC production/speed
builds. All generated compute variants pass glslang and spirv-val for Vulkan 1.2,
including hardware generation, diffuse, specular and query-diagnostic variants.
The relocated MSME source is compiled directly from Kiln's shader source
directory. Existing C# API signatures are unchanged.

```powershell
$deps = (Resolve-Path bin/build_deps).Path
python -m SCons platform=windows target=editor module_mono_enabled=yes arch=x86_64 production=yes dev_build=no optimize=speed debug_symbols=no -j16 "mesa_libs=$deps/mesa" "angle_libs=$deps/angle" "accesskit_sdk_path=$deps/accesskit" "agility_sdk_path=$deps/agility_sdk" "pix_path=$deps/pix"
```

Repeat with `target=template_release`. SCons regenerates the combined GI shader
from stage/include changes. Headless asset import/export is not visual validation.

### MSME source relocation validation (2026-09-18)

- Implemented: removed the unused SurfelPlus reference snapshot and moved the
  compiled MSME implementation plus its required notices into the Kiln renderer.
  The MSME function body is unchanged; only attribution comments and the final
  newline differ from the retained snapshot.
- Compiled: every generated Vulkan compute variant passed `glslangValidator` and
  `spirv-val`; the macOS arm64 editor built and linked successfully.
- Visually verified: a real 960x540 Metal window rendered Sponza on Apple M5 with
  native hardware ray queries, produced the expected lit scene, and captured
  finite GI resources under `/tmp/kiln-msme-relocation.6cxk3A/`.
- Unsupported/unverified: this relocation was not rebuilt on Windows/Linux and
  makes no visual-quality or performance claim because it changes no shader logic.

## Pre-optimization real GPU validation

Host: Windows / NVIDIA GeForce RTX 5070 Ti / Vulkan 1.4.341, 2026-09-17.
New captures and logs live in `%TEMP%/kiln-deferred-specular/`; previous visibility
results in `%TEMP%/kiln-surfel-review/` and `surfel-validation.json` are historical.
They do not validate this G-buffer/specular revision.

The final editor combined acceptance at 960x540 passed import/serialization,
resource invalidation/lifecycle, diffuse multibounce, instance/normal diagnostics,
odd-size resize, isolated lighting, motion and TOD checks. The wet-floor specular
suite passed all 223 numerical checks. Stationary display-RGB standard deviation
was 0.001999 (limit 0.005), and motion-settled/reference MAE was 0.001338 (limit
0.035). Offscreen-emitter acceptance requires zero primary emissive pixels while
its blue reflection remains visible. Zero-F0 and metallic captures check material
composition, rather than only nonzero ray output.

| Run | Verified result | Evidence |
| --- | --- | --- |
| Editor combined, 960x540 | 105 G-buffer/multibounce, 276 light/motion, 223 reflection and 25 TOD checks; import and resource lifecycle passed | `final-editor/` |
| Embedded-PCK release, 1920x1080 | 224 reflection checks passed, including odd-size resize; display noise 0.001397, settled/reference MAE 0.001193 | `release-1080/` |
| Forced compute BVH, 480x270 | 160 real-window frames; 10 diffuse checks plus finite/nonzero specular; actual backend software | `software/` |
| Forward+ and deferred, 960x540 | Identical still camera, TAA and ray budgets; finite/nonzero specular; display-RGB MAE 0.003165, descriptive only | `forward/`, `deferred/` |
| Material contract chart | Texture, cutout, normal map, transparency/additive/refraction render in both paths; display-RGB MAE 0.000152 | `material-*/` |

Accepted logs contain no engine/script errors or Vulkan validation errors.
Hardware/software query diagnostics report zero mismatches. Visual inspection
separately covered wet Sponza, sharp and rough emissive reflections, offscreen
blue reflection, zero-F0, metallic, turn-off, alternate backends and the material
chart. Isolated specular still has Monte Carlo grain at the default two-ray budget;
it is not claimed to be noise-free.
Source and binary hashes, measurements and limits are recorded in
[deferred-specular-validation.json](deferred-specular-validation.json).
These are functional checks with diagnostic readbacks, not a fair performance benchmark. No FPS,
1080p/60 target, reduced-bandwidth or other-platform result is claimed.

## Pre-optimization performance measurement

A separate run without validation layers or readbacks measured the current 1080p
release on the same RTX 5070 Ti. Static full GI is approximately 10.1 FPS, diffuse
GI alone 47.2 FPS, and GI disabled 463 FPS (same AO/shadows). The 60 FPS target is
not met. These figures are distinct from the functional acceptance runs above.
See [conditions, repeats and raw frame data](PERFORMANCE-2026-09-17.md).

## Current optimization and validation

The compact-grid, allocation and reflection optimizations are separately
documented in [GI-OPTIMIZATION-2026-09-17.md](GI-OPTIMIZATION-2026-09-17.md).
Repeated release measurements at 1080p are 157–159 FPS with full GI on RTX 5070 Ti.
That report records individual GI GPU passes, full-rate quality comparisons,
extended camera/TOD movement, shader/build checks and new real-GPU acceptance.
The earlier 10.1 FPS baseline above remains historical evidence.

The [stationary smoothness revision](GI-SMOOTHNESS-2026-09-17.md) adds stratified
diffuse sampling, corrected cache convergence and geometric spatial filtering.
Its new GPU verification and timings supersede the earlier quality/performance
results for this revision; unfiltered cache and filtered output remain separate.

## Unsupported or unverified

- Secondary transport admits opaque BaseMaterial3D and explicit uniform custom
  material contracts. Alpha cutout/blended secondary geometry is still omitted;
  primary raster cutout/transparency continues to render. Secondary emission
  textures use their linear average, and secondary normal maps, UV2/triplanar
  projection and arbitrary custom UV/shader logic are not evaluated.
- Glossy hits terminate at emission plus direct/diffuse-cache radiance. Recursive
  mirror-in-mirror/specular chains, transmission, caustics, anisotropy and clearcoat
  GI are not implemented. Offscreen cache misses use the traced diffuse fallback;
  there is no claim that all unseen surfaces retain converged multibounce history.
- Skinned/blend-shape/MultiMesh ray geometry and GPU-only deformation. Rigid
  MeshInstance3D transforms and material overrides are supported.
- Linux/D3D12 GPU verification; Metal configurations outside the scope recorded
  in [METAL-RAY-QUERY.md](METAL-RAY-QUERY.md); other vendors;
  XR/MSAA, reflection-probe capture, SDFGI/VoxelGI/lightmap mixing. Deferred's
  existing explicit feature rejections remain documented in PIPELINE.md.
- Performance targets and all platform passes not explicitly listed above.

## Reproduce

```powershell
python kiln/tools/check_surfel_shaders.py
$engine = './bin/godot.windows.editor.x86_64.mono.console.exe'
& $engine --path kiln/sponza --rendering-driver vulkan --rendering-method kiln_deferred --gpu-validation -- --specular-suite --size=960x540 "--output=$env:TEMP/kiln-specular-check"
python kiln/tools/check_specular.py "$env:TEMP/kiln-specular-check"
python kiln/tools/validate_sponza.py --engine $engine --size 960x540 --output "$env:TEMP/kiln-deferred-acceptance"
```

Numerical acceptance, actual screenshots and performance measurements are separate.
Imported Sponza asset provenance and redistribution limits remain in its README.
