# Diffuse surfel cache: GIBS review and GPU verification

This revision focuses on indirect diffuse under sun/sky illumination. Local
lights, emissive workloads and indirect specular quality are outside this
verification. Resolution, material albedo, GI intensity, original scene geometry
and camera timelines are preserved. No half-resolution diffuse path was retained.

## What the original presentation actually proposes

Primary reference: Halen, Hayward, Brinck and Bei, EA, SIGGRAPH 2021,
[Global Illumination Based on Surfels](https://advances.realtimerendering.com/s2021/SIGGRAPH%20Advances%202021%20-%20Surfel%20GI.pdf).
Page numbers below are PDF pages, including speaker notes.
The entire text was reviewed; the sharing and budget diagrams were rendered
and inspected. The presentation is a design reference, not public Frostbite code.

| Original technique | Relevance and current implementation |
| --- | --- |
| Irradiance sharing, p.116-122 | The diagram places spatial sharing **between ray tracing and MSME**, with stronger sharing at high variance. Implemented in this order. The earlier experimental filter of accumulated means was removed. |
| Adaptive requests and global budget, p.75-79 | New/uncertain surfels receive greater priority. GPU prefix allocation redistributes the nominal request and limits the primary-ray total. Implemented. |
| Ray guiding, p.80-115 | A discrete hemispherical radiance distribution directs samples toward useful illumination. Implemented with 4x4 cosine-domain bins, a 25% uniform component and explicit PDF correction. The original normally uses 6x6 compressed bins. |
| Recycling, p.25-29 | Preserve useful history but reclaim entries under memory pressure. Implemented bounded offscreen retention and a shorter retirement threshold above 80% occupancy. This is simpler than the original probabilistic, distance-aware heuristic. |
| Radial depth moments, p.41-59 | A small hemispherical mean/second-moment depth map rejects lighting across occluders. **Not implemented here.** Current normal/plane rejection is less general, especially around corners and nearby disconnected surfaces. This is the next geometric quality improvement. |
| Spatial/directional ray binning, p.123-126 | Improves coherent ray execution. **Not implemented here.** It needs a ray queue and measured sorting overhead; the current shader traces a batch per surfel. |
| Nonlinear spatial grid, p.30-40 | The original adapts cell shape to distance. Kiln retains its complete, three-level hashed overlap lists, checked against brute force. No unvalidated grid replacement was introduced. |

Also reviewed [W298/SurfelGI](https://github.com/W298/SurfelGI) at
`8361942f7d799632b32d37356b8057814456a8a2` (MIT). Its integration shader illustrates
variance-controlled neighboring irradiance reuse and depth rejection. No source
was copied from that repository. Existing SurfelPlus and MSME imported files and
their license notices remain byte-for-byte unchanged; the hash check passes.

## Implemented engine changes

The diffuse path is now:

`update / spawn -> ray requests -> guided trace -> irradiance sharing -> MSME -> full-resolution gather -> two small reconstruction passes -> temporal publish`

- Sharing reads immutable independent ray batches, at most four frames old, to
  include staggered neighbors. A bounded tangent-plane kernel uses complete
  overlap queries and symmetric surface-plane/normal rejection. Relative
  variance reduces sharing on stable estimates. Filtered means are not repeatedly
  fed into this spatial filter. This is a spatial approximation: it can soften
  lighting within its support, and is not an unbiased path-tracing estimator.
- MSME initialization uses the signal's scale; its unchanged imported equations
  are evaluated in scaled irradiance units so absolute variance/firefly floors
  do not dominate dark receivers. Lighting-change decay accounts for elapsed
  frames between updates. Progressive sample indices advance only on actual rays.
- A cache miss continues one bounded diffuse segment instead of falling back
  immediately to sky-only illumination. Shadow and continuation work is included
  in timings, but is **not** included in the primary-ray counter/budget.
- Mature surfels stagger updates; new entries converge before decimation.
  The default primary budget is 196,608 per frame. Allocation changes sample
  density, not radiance magnitude; normalization uses actual samples and PDFs.
  This is explicitly a quality/performance tradeoff, not identical sampling.
- Offscreen retention no longer fills the pool indefinitely during camera turns.
  The prior experiment reached the pool limit and lost visible coverage; pressure
  reclamation restores room for newly exposed surfaces without a CPU readback.
- Four wide screen diffuse filters became two smaller filters with shared halo
  reconstruction. `--raw-cache` bypasses these and temporal publish. Cache
  irradiance sharing remains separately switchable.
- Disabled indirect specular no longer dispatches specular tracing/filtering.
  NRD no longer launches screen diffuse rays or suppresses the cache resolve.
  Switching NRD does not destroy diffuse history. The adapter declares two
  `RELAX_SPECULAR` Schlick signals; cache irradiance has no fabricated first-hit
  distance. **NRD diffuse denoising is not implemented by this revision.**

## Compiled

Windows x86_64 Mono production editor and `template_release`, MSVC speed builds.
Every native GLSL variant passes glslang and spirv-val, with imported-source hashes
verified. An embedded Sponza release was exported and rendered on the real GPU.
The final comment/macro cleanup produced byte-identical SPIR-V to the tested
candidate; both engine targets were rebuilt afterward.

## Real GPU verification

Windows, NVIDIA GeForce RTX 5070 Ti, Vulkan 1.4.341. Real windows, not headless
startup. Validation/capture runs are separate from timing. Headless was used only
for export. Output is under `G:/Temp/kiln-surfelgi-final-20260917`; earlier
experiments and the original dirty-source snapshot remain in
`%TEMP%/kiln-surfelgi-audit`.

- 1080p sun/sky diffuse: 132 existing structural checks and 102 focused checks
  pass, including finite/nonnegative output, ray-query validation, camera turns,
  return to the original view, relighting, 961x541 recreation and restored size.
- 960x540 raw sharing ablation: 115 focused checks pass. Screen and temporal
  reconstruction are disabled in both variants. Shared-batch validity, contiguous
  GPU allocation prefixes, actual ray totals and budget limits are checked.
- The moving 1080p grid passes 2,048 independent brute-force support queries.
  Minimum positive-support coverage over captured 1080p states is 99.988%.
  Positive support is not proof of correct illumination at every pixel.
- Visual inspection includes isolated linear diffuse at still/converged/moving
  viewpoints, shared versus unshared raw output and the final embedded lit image.
  Residual floor/wall mottling remains, particularly while exposing new surfaces.

### Sharing ablation, same revision and settings

Fixed planar floor patch, 960x540, approximately 240 settled frames:

| Measurement | Sharing off | Sharing on |
| --- | ---: | ---: |
| Mean linear diffuse irradiance/pi | 0.0440123 | 0.0450288 |
| Relative spatial high-pass residual | 0.0634255 | 0.0281616 |

The raw residual falls **55.6%**, with mean energy **+2.3%**. This passes the
unchanged 35% reduction criterion for this explicit raw-sharing ablation.
The earlier starting-source raw residual was 0.0801577; the complete cache
revision is about 64.9% lower against that older capture. These are different
comparisons, not two estimates of sharing alone.

The metric includes genuine lighting gradients; it is not error against a
path-traced reference. At 1080p raw-gather residual falls from 0.0297515 at the
first still capture to 0.0202985 after longer convergence. The isolated-indirect
display's stationary RGB temporal standard deviation is 0.000683; this includes
TAA and display quantization, not just cache instability.

The historical **final filtered image** gate must be reported separately. At
960x540 the old four-pass filtered residual was 0.0226064; the final two-pass
candidate is 0.0230471, about **1.9% higher**, with mean light +3.4%. Thus the
original requirement for a 35% improvement of the final filtered output still
**fails**. Its threshold was not relaxed or replaced by the successful raw-cache
ablation. The cache is substantially better and needs less reconstruction, but
this does not establish that the final-image blotch problem is solved.

### Budget comparison

The matching unrestricted 1080p run uses the same adaptive weighting and sharing,
with `--surfel-ray-budget=0`. Across the recorded still/sunset/camera/return states,
mean diffuse ratios are 0.997-1.015. Per-pixel normalized MAE is 5.0-11.6%; even
the two uncapped static states differ by 5.8% because GPU allocation and sampling
are not bitwise deterministic. This is a convergence comparison, not proof of
identical quality. Full readbacks and `budget-comparison.json` retain the differences.
For example, sunset uses 196,608 primary rays instead of 307,872.

## Performance

Embedded production release, full 1920x1080, TAA, sun/sky, no NRD or indirect
specular. Each run has 180 warmup frames plus 1,020 measured frames. No GPU
readbacks, validation layers or profiling during these runs; cases run sequentially.
Rendering frame counters confirm the windows continued rendering.

| Workload | Whole frame mean ms | P95 ms |
| --- | ---: | ---: |
| Deferred diffuse, static | 4.875 | 5.272 |
| Static repeat | 4.861 | 5.277 |
| Matched static GI off | 1.809 | 2.051 |
| Deferred diffuse, camera + sunlight moving | 5.829 | 6.666 |
| Matched moving GI off | 1.882 | 2.161 |
| Moving, unrestricted primary rays | 8.064 | 9.957 |
| Forward+, same diffuse settings, static | 6.489 | 7.416 |

The whole-frame GI-on/off difference is **3.06 ms static** and **3.95 ms moving**.
It includes surfel management, tracing, sharing, reconstruction and integration
overhead; it is not a single isolated GPU timestamp. The current few-ms result
applies to diffuse and these measured workloads only. It does not establish a
full diffuse-plus-specular-plus-NRD budget or a worst-case guarantee.

Separate moving GPU profiling identifies roughly 1.17 ms tracing, 0.95 ms sharing,
0.80 ms screen reconstruction and 0.23 ms temporal publish in the last reported
interval. The request pass is about 0.021 ms. These are illustrative profiled
intervals, not averages from the unprofiled performance table. Sharing itself is
now a significant cost; ray binning cannot remove that cost.

## Reproduction and unverified scope

Use `kiln/sponza/run.ps1 -NoNRD -NoSpecular` for interactive diffuse. For raw cache
inspection, add `--raw-cache` to the executable's demo arguments after `--`;
`--no-irradiance-sharing` and `--surfel-ray-budget=0` expose the two independent
tradeoffs. Captures report these settings and actual primary-ray counts.

Run `res://tests/diffuse_focus.gd` in a real window, then
`kiln/tools/check_diffuse_focus.py <capture-root> --without-sharing <ablation-root>`.
The optional ablation argument is for equal-size raw-cache captures.
Use `kiln/tools/benchmark_sponza.py --engine <embedded-exe> --embedded
--diffuse-only --frames 1200 --long-frames 0 --output <temp-directory>` for matched
timing cases. All commands accept output locations outside the source tree.

Not validated here: active NRD/reflection quality, local/emissive workloads,
native Metal, D3D12, Linux, AMD/Intel GPUs or compatibility/mobile GI. Existing
unsupported material/geometry features remain unsupported. Depth-moment
visibility and ray binning from the original presentation remain future work.
The earlier broad suite had failures before this focused revision; its historical
results are not substituted for a rerun of those excluded cases.
