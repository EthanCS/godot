# Metal GI: optimization following Xcode capture, 2026-09-17

This checkpoint follows the actual M5 GPU capture, separately from the earlier
[Metal optimization checkpoint](METAL-GI-OPTIMIZATION.md). Reflection tracing
remains the largest GPU workload. The changes reduce its instruction count;
they do not establish a 60 FPS result.

## Implemented

- The translated reflection shader groups receivers by checkerboard parity.
  Adjacent lanes trace the two independent sample streams of one receiver.
  On Apple's 32-lane SIMD groups, rough-surface omissions can retire a whole
  group instead of leaving half of both groups idle. The 16×2 receiver tile,
  pixel coordinates, random seeds, ray budget and accumulation order remain
  unchanged. The mapping also covers odd viewport borders and 1–8 ray budgets.
- Hardware visibility rays use a dedicated opaque, terminate-on-first-hit
  query, requesting only the hit bit. Closest-hit shading retains its original
  triangle, distance and normal results. The scene query diagnostic now checks
  the dedicated visibility path against software BVH and the general query.
- Shared surfel buffers declare read-only access in stages that only read them.
  Allocation, integration, atomic counters and grid writers retain write access.
  This lets RD/Metal distinguish actual writes from independent readers.

Resolution, diffuse/specular sample counts, materials, filter thresholds and
temporal history lengths were not reduced. A coverage-query early-exit trial
showed no stable benefit and was removed. The optional handwritten MSL reflection
shader is unchanged; measurements below use the default translated GLSL path.

## Xcode evidence

Both captures use the same 1280×720 paused-noon Sponza view, wet floor, full GI,
TAA, hardware ray queries and two diffuse/two specular rays. They contain 158
draws and 29 compute dispatches. Full/Overlapping/Maximum profiling reports:

| Reflection trace counter | Before | After |
| --- | ---: | ---: |
| Kernel ALU instructions | 7,991,257,920 | 6,473,778,352 |
| RT intersect ray threads | 1,329,394 | 1,329,191 |
| Device-memory bytes read | 555,159,744 | 587,140,736 |
| Kernel occupancy | 41.37% | 52.99% |

The captured reflection ALU count is 19.0% lower with nearly identical traced
ray counts. Device reads increase 5.8%, a tradeoff rather than a bandwidth win.
Unchanged AO/filter ALU counts differ by less than 0.1%. These observations
support reduced reflection work, not a corresponding whole-frame FPS claim.

Effective GPU time reads 36.08 ms before and 19.30 ms after, but Xcode sampled
3/10 versus 7/10 cores. **Those times are not a valid speedup ratio.** Capture
also enables Metal API validation. Encoder cost percentages are not absolute
pass timings. Production measurements below run outside Xcode with validation
and profiling disabled.

## Measured frame intervals

Both comparison executables were built with the same Xcode 27/macOS 27 SDK.
The original executable used SDK 26.2, so it is retained in temporary storage
but excluded from this comparison. Real windows remain visible; rendered-frame
counters are checked. Startup and the first 240 frames are excluded.

| Workload | Control ms / FPS | Optimized ms / FPS |
| --- | ---: | ---: |
| Paused noon, 600 frames | 22.254 / 44.94 | 20.946 / 47.74 |
| Camera/TOD movement, 1,800 frames | 28.536 / 35.04 | 27.728 / 36.06 |
| Multi-light, 600 frames | 28.449 / 35.15 | 27.018 / 37.01 |
| Paused noon repeat, 600 frames | 20.302 / 49.26 | 21.218 / 47.13 |

The first three runs improve mean FPS by 2.9–6.2%; the static repeat regresses
4.3%. Moving-camera p95 also increases from 35.393 to 36.715 ms. Other adjacent
static trials measured 22.360 ms control versus 20.515/19.678 ms optimized.
The shared desktop shows enough variability that these results do not establish
a stable whole-frame gain or better tail latency. No fixed overall speedup is
claimed. Reflection traversal and memory traffic remain substantial bottlenecks.

## Validation and limits

The macOS arm64 editor, release template and all 24 GI GLSL/SPIR-V variants
(including native-interface metadata) compiled successfully.
The editor passed real-window acceptance with Metal API validation: 115 surfel,
300 scene, 223 reflection and 25 TOD checks, original-mesh import and lifecycle
checks. The dedicated visibility query is included in scene-query parity.
Actual 961×541 render targets are exercised.

With both Metal API and shader validation, ray-count changes 2 → 1 → 3 → 8 → 2
also passed at 481×271 on hardware ray queries and the software BVH. Reflection
display-noise standard deviation was 0.00237684 before and 0.00238637 after;
32-frame settling MAE was 0.00147216 before and 0.00147240 after. Both versions
passed the original thresholds; these are regression checks, not quality gains.

The exported release PCK passed all 223 reflection checks at 960×540, including
the 961×541 transition, with Metal API and shader validation enabled. Its
Forward+ path completed a real 1280×720 launch/capture under API validation.

The optimized Xcode drawable, odd-size daylight/wet-floor output and blue-emitter
reflection were visually inspected. Numerical acceptance and visual inspection
are separate from performance measurement. Low-ray noise remains visible.
Release wet-floor output and the Forward+ drawable were also inspected.

Raw captures, CSV counters, build logs, exact control/optimized executables and
benchmark JSON remain in `/tmp/kiln-xcode-opt-20260917`; the original Xcode frame
is in `/tmp/kiln-xcode-frame-20260917`. The accompanying machine-readable record
([JSON](metal-gi-xcode-followup.json)) identifies the exact binaries and source
hashes.

Vulkan/D3D12, other operating systems, other Apple GPUs and Intel/AMD Macs were
not rebuilt or GPU-tested for this checkpoint. Native MSL scheduling is not
optimized here. Neither 1080p nor sustained 60 FPS is validated. Existing
[transport and geometry limits](SURFEL-GI.md) still apply.

## Reproduction

```sh
python3 kiln/tools/check_surfel_shaders.py --output /tmp/kiln-gi-shaders
scons platform=macos target=editor arch=arm64 vulkan=no metal=yes -j8
scons platform=macos target=template_release arch=arm64 vulkan=no metal=yes -j8
MTL_DEBUG_LAYER=1 python3 kiln/tools/validate_sponza.py \
  --engine bin/godot.macos.editor.arm64 --driver metal --size 480x270 \
  --output /tmp/kiln-gi-acceptance
```

Release testing places the exported PCK beside an identically named executable;
the default release template disables command-line path overrides.
