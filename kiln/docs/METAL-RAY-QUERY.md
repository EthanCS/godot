# Metal Surfel GI ray queries

## Implemented

The existing Surfel GI compute passes now use native Metal triangle acceleration
structures on hardware supporting Apple GPU family 9 (M3/A17 Pro or later).
The driver also requires `supportsRaytracing`, MSL 2.4 or later, and macOS 14 /
iOS 17 / tvOS 17 / visionOS 2 or later. This is an implementation gate, not a
claim that every listed platform has been tested.

- Original-mesh triangle BLAS and instance TLAS construction use
  `MTLAccelerationStructureCommandEncoder` on the rendering command buffer.
- Indexed/non-indexed float3 positions, vertex/index offsets and strides, instance
  transforms, masks, opacity/culling flags, custom IDs and placeholder indices
  are translated into Metal descriptors. RD's deferred resource lifetime applies.
- SPIRV-Cross translates the existing GLSL ray queries into MSL. Proven opaque
  triangle queries use native `intersector`; queries needing candidates or
  unsupported flags/getters retain `intersection_query`. Metal shader reflection
  and argument/direct bindings include the TLAS. BLAS residency is declared at
  binding time, after queued TLAS builds execute.
- Tracked Metal hazards order uploads, BLAS/TLAS builds and ray-query dispatches.
  The experimental `GODOT_MTL_FORCE_BARRIERS=1` mode selects compute BVH instead.
- G-buffer geometric normals use explicit GLSL SNORM packing. On the tested
  Metal compiler, the native `pack_float_to_snorm2x16` fragment intrinsic
  produced a zero normal channel; explicit packing restores surfel anchors.
- GI allocation, diffuse multibounce, shadow visibility and GGX specular reuse
  the existing hardware-query shaders. The software BVH is retained. No GI
  transport algorithm or Forward+/Mobile/Compatibility raster path was replaced.

Select Metal using `--rendering-driver metal`. On eligible hardware automatic
query selection uses native Metal ray queries. The Sponza `--hardware` option
requests this backend; `--software` selects compute BVH. Always inspect actual
`backend` / `hardware_ray_query_available` statistics to detect fallback.

The subsequent [Metal optimization report](METAL-GI-OPTIMIZATION.md) records the
optimized builds, repeated performance measurements and new GPU validation. The
initial support checkpoint below retains its original hashes and evidence.

## Initial build and validation

On 2026-09-17, macOS 26.6.2 / Apple M5 / Metal 4, the arm64 editor and release
template built successfully with Metal enabled and Vulkan/.NET disabled. All 23
GI GLSL/SPIR-V variants passed glslang and spirv-val. These builds do not establish
Windows, Linux or mobile build compatibility.

The editor completed the Sponza real-window acceptance suite at 480x270, including
961x541 render-target transitions: 115 surfel comparison checks, 300 scene checks,
223 specular checks, 25 time-of-day checks, original-mesh import and 13 lifecycle
records. Metal API validation was enabled. The focused ray-query regression
passed 18 ray cases each with argument buffers and direct bindings, with both
Metal API and shader validation enabled.

The release template ran an exported Sponza PCK from temporary storage at 960x540
with Metal API and shader validation enabled; all 223 specular checks passed,
including the 961x541 transition. The data-only PCK used the existing desktop
export preset; no macOS app bundle or distributable was published.

A paused-noon capture reported `backend=hardware_ray_query`, 2,048 diagnostic
rays, 1,916 hits, zero mismatches and maximum hit-distance error 0.00000763.
Surfel receiver coverage was 99.9992%. Explicit software BVH and equal-quality
Forward+ comparison captures also completed. These are functional checks, not
performance measurements.

## Visually inspected

Real-GPU captures show indirect light on Sponza's walls and floor, varied G-buffer
geometric normals, colored emitter reflections, offscreen reflection transport,
zero-F0 suppression, metallic reflection, camera movement and dusk/night changes.
The release PCK's wet-floor daylight and red-emitter reflection images were also
inspected at 960x540.
The default low-ray reflection captures retain visible stochastic noise; this
validation does not claim a new quality or performance target.

Captures and logs remain in `/tmp/kiln-metal-validation` and `/tmp/kiln-metal-*.log`.
The compact record, including binary/source hashes and scope, is
[metal-ray-query-validation.json](metal-ray-query-validation.json).

## Reproduction

```sh
scons platform=macos target=editor arch=arm64 vulkan=no metal=yes -j8
scons platform=macos target=template_release arch=arm64 vulkan=no metal=yes -j8
python3 kiln/tools/check_surfel_shaders.py --output /tmp/kiln-metal-validation/shaders
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 bin/godot.macos.editor.arm64 \
  --path kiln/sponza --rendering-driver metal --rendering-method kiln_deferred \
  --script res://tests/metal_ray_query.gd
python3 kiln/tools/validate_sponza.py --engine bin/godot.macos.editor.arm64 \
  --driver metal --size 480x270 --output /tmp/kiln-metal-validation/acceptance
```

The focused GPU test checks indexed/non-indexed BLAS geometry, nonzero buffer
offsets, padded strides, transformed instances, masks, custom and built-in IDs,
primitive/geometry IDs, hit distances, barycentrics and repeated TLAS rebuilds.
It is separate from real-window GI captures and visual inspection.

## Unsupported or unverified

- Ray-generation/miss/hit shader pipelines and shader binding tables are not
  implemented. `SUPPORTS_RAYTRACING_PIPELINE` remains false; Surfel GI uses
  `SUPPORTS_RAY_QUERY`.
- Ray geometry formats other than float3 are rejected. Existing GI restrictions
  on cutout/translucent secondary geometry, skinning, GPU deformation and recursive
  specular transport remain as documented in [SURFEL-GI.md](SURFEL-GI.md).
- Hardware queries on older Apple GPUs and the opt-in untracked barrier mode
  are disabled; software BVH remains available.
- iOS/tvOS/visionOS, Intel/AMD Macs and other Apple GPU models require separate
  builds and real-GPU validation. No macOS performance target is claimed.

API references: [Apple acceleration structures](https://developer.apple.com/documentation/metal/ray-tracing-with-acceleration-structures),
[Metal feature tables](https://developer.apple.com/metal/capabilities/),
[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross).
