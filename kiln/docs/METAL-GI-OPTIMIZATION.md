# Metal GI optimization — 2026-09-17

## Implemented

The original Metal support checkpoint is retained in [METAL-RAY-QUERY.md](METAL-RAY-QUERY.md).
This update improves the generated Metal program and reflection scheduling:

- Proven opaque, triangle-only ray queries lower to Metal `intersector` instead
  of the general `intersection_query`. The compiler conservatively checks flags
  and result operations, preserves traversal timing and initial committed state,
  and retains the general path for candidate processing or unsupported cases.
  Triangle-data payloads are included only when requested. The shader cache
  format was bumped so previous binaries cannot silently retain old MSL.
- Two reflection sample lanes process each receiver independently, with 32
  receivers per 64-thread group. Ray counts 1–8 use cached specialized pipelines.
  The ray budget and BRDF estimator are unchanged. Independent random streams
  remove the dependency between successive rays' hit lighting; stochastic sample
  patterns consequently change, so output is not bit-identical.
- The reflection filter reconstructs positions/normals once per shared tile and
  halo, retaining its original neighbors, rejection thresholds and weights.
  Border threads participate in the barrier before exiting.
- Fast-trace acceleration-structure requests map to Metal's fast-intersection
  build hint on OS 26 or later. Older systems retain the default policy.
- Benchmark windows stay visible and stale rendered-frame counters fail the
  runner. A development run with an occluded window was discarded; its apparent
  speedup was not real. Temporary GPU-counter instrumentation was removed.

GLSL is compiled through SPIR-V to MSL before execution; there is no per-ray
translation at runtime. The useful change is the generated operations and their
resource use, not the source-language extension. An intermediate native-query-only
measurement improved about 48 to 51 FPS. The remaining gains required reflection
work reduction. Shared radiance tiles and a compact gather-cache experiment did
not improve the measured workload and were reverted.

## Measured performance

Apple M5, macOS 26.6.2, Metal 4, arm64 production editor builds, 1280×720, TAA,
two diffuse and two specular rays, existing rough-reflection checkerboard policy.
No validation layers, GPU-counter instrumentation or diagnostic readbacks ran
during measurements. Before/after processes ran sequentially with the same scene
script and settings. Frame times are wall-clock frame intervals, not GPU-stage
timestamps. Startup and the first 240 frames are excluded.

| Workload | Before ms / FPS | After ms / FPS | FPS change |
| --- | ---: | ---: | ---: |
| Paused noon, full GI | 20.868 / 47.92 | 17.469 / 57.24 | +19.5% |
| Paused noon, repeated | 20.890 / 47.87 | 17.519 / 57.08 | +19.2% |
| Moving camera / TOD, 1,800 frames | 26.416 / 37.86 | 23.066 / 43.35 | +14.5% |
| Paused noon, diffuse only | 11.665 / 85.73 | 10.509 / 95.16 | +11.0% |
| Paused noon, GI off | 8.333 / 120.00 | 8.335 / 119.98 | unchanged |

Static runs contain 600 frames, with 360 measured. Moving runs contain 1,560
measured frames; their p95 interval decreases from 32.642 to 28.719 ms and maximum
from 35.065 to 30.701 ms. Rendered-frame counters reached 598 and 1,798 respectively.
These bounded runs establish neither sustained thermal behavior nor a 60 FPS
dynamic-scene target. No 1080p performance result is claimed for this update.

The exported release PCK measured 17.456 ms / 57.29 FPS in deferred and
22.568 ms / 44.31 FPS in Forward+ at the same static camera, resolution, TAA
and GI settings. Both used the same optimized release executable and 600-frame
protocol. This is an equal-quality renderer comparison, not an additional
before/after optimization result.

## Compiled and GPU validation

The macOS arm64 editor and release template built with Metal enabled and
Vulkan/.NET disabled. All 23 GI GLSL/SPIR-V variants passed validation.

The optimized editor passed real-window acceptance with Metal API validation:
115 surfel checks, 300 scene checks, 223 reflection checks, 25 TOD checks,
original-mesh import and 13 lifecycle records. Tests include actual 961×541
render targets. The unchanged reflection thresholds cover stationary noise,
movement settling and light-off decay. At 480×270, display noise standard
deviation was 0.002382 before and 0.002385 after; settling MAE was 0.001467 before
and 0.001444 after. These are regression observations, not a quality improvement
claim.

Native opaque and general candidate queries each passed 18 transformed/indexed/
masked/rebuilt ray cases with both argument buffers and direct bindings. Metal
API and shader validation were enabled. Initial committed state was checked
before traversal. The dedicated ray-budget regression exercises 2 → 1 → 3 → 8 →
2 rays at 481×271, including cached-pipeline reuse and finite/nonzero results,
with both hardware queries and software BVH under Metal API/shader validation.

The exported release PCK also passed all 223 reflection checks at 960×540,
including a 961×541 transition, with Metal API and shader validation enabled.

## Visually inspected

Real-GPU optimized editor captures show wet-floor indirect reflections and blue
reflection from an offscreen emitter. Original surfel allocation, diffuse
lighting and temporal reflection checks passed separately from image inspection.
The release wet-floor daylight and red-emitter reflection captures were also
inspected at 960×540. Low-ray stochastic noise remains visible.

Raw logs, captures and benchmark data remain in `/tmp/kiln-metal-perf`. The compact
[validation and timing record](metal-gi-optimization.json) identifies the exact
source and binary hashes. The initial support record is historical and has not
been relabeled as validation of these optimized binaries.

## Unsupported or unverified

The shared GLSL changes have not been rebuilt or GPU-tested on Vulkan, D3D12,
Windows or Linux in this update. Mobile Apple platforms, Intel/AMD Macs and other
Apple GPU models remain unverified. Existing material/geometry/transport limits
in [SURFEL-GI.md](SURFEL-GI.md) still apply. Native RT shader pipelines remain
unsupported. The complete dynamic Sponza workload remains below 60 FPS on this M5.

## Reproduction

```sh
scons platform=macos target=editor arch=arm64 vulkan=no metal=yes -j8
scons platform=macos target=template_release arch=arm64 vulkan=no metal=yes -j8
python3 kiln/tools/check_surfel_shaders.py --output /tmp/kiln-metal-shaders
python3 kiln/tools/benchmark_sponza.py --engine bin/godot.macos.editor.arm64 \
  --driver metal --size 1280x720 --frames 600 --warmup 240 --long-frames 1800 \
  --output /tmp/kiln-metal-benchmark
MTL_DEBUG_LAYER=1 python3 kiln/tools/validate_sponza.py \
  --engine bin/godot.macos.editor.arm64 --driver metal --size 480x270 \
  --output /tmp/kiln-metal-acceptance
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 bin/godot.macos.editor.arm64 \
  --path kiln/sponza --rendering-driver metal --rendering-method kiln_deferred \
  --script res://tests/metal_ray_query.gd
# Add -- --query-candidates for the general-query path. Set
# GODOT_MTL_DISABLE_ARGUMENT_BUFFERS=1 to test direct bindings.
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 bin/godot.macos.editor.arm64 \
  --path kiln/sponza --rendering-driver metal --rendering-method kiln_deferred \
  --script res://tests/specular_ray_counts.gd -- --still \
  --output=/tmp/kiln-metal-ray-counts
# Add --software to test compute BVH with the same ray-budget transitions.
```

Apple's [ray-tracing performance guidance](https://developer.apple.com/videos/play/tech-talks/111373/)
describes intersectors and minimizing intersection payloads. Measurements above,
rather than API selection alone, establish the observed improvement.
