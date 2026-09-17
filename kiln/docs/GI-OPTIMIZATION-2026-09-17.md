# Surfel GI optimization — 2026-09-17

This report supersedes the [98.84 ms pre-optimization baseline](PERFORMANCE-2026-09-17.md).
Measurements apply to Windows/Vulkan on the RTX 5070 Ti, with the self-contained
Sponza scene. They do not establish parity with the original SurfelPlus sample,
which uses different transport, scene and glossy reconstruction.

## Implemented

- Replace 81 neighboring hashed-cell lookups and linked-list traversal with
  complete compact overlap lists. Each surfel is inserted into the cells its
  support sphere and thin plane can intersect. GPU count, hierarchical prefix
  sum and scatter passes produce contiguous lists; each query visits one bucket
  per level. Collisions add candidates, without truncating crowded cells.
- Reject candidates by radius before fetching normals or irradiance. Allocation
  queries load coverage only. Update visibility once per surfel, replacing
  contended per-pixel atomic writes in the full-resolution diffuse resolve.
- Deduplicate allocations in world space within each frame. Multiple screen
  tiles can otherwise allocate thousands of surfels on the same small surface
  near the camera, because their shared input grid predates all new allocations.
  Colliding claims defer allocation until the next coverage test; they do not
  remove existing contributions or impose a per-cell gather limit.
- Sample the same 512x512 albedo pages through an sRGB texture array, using
  hardware decoding and bilinear filtering. UV transforms, repeat and clamp
  behavior remain supported. Resources are released on viewport reconfiguration.
- Alternate half-rate GGX samples on rough receivers (roughness >= 0.45) only
  when paired positions, normals and roughness agree. The existing spatial and
  temporal reflection filter reconstructs missing samples. Sharp/wet surfaces
  and discontinuities retain full-rate rays. This reconstruction is an
  approximation; `--full-specular-rate` disables it for a reference comparison.
- Expose individual GPU pass timestamps through `--gpu-profile`; add independent
  compact-grid readback validation and a repeatable paired benchmark runner.

Diffuse ray budgets, multibounce, surfel support weights, resolution, TAA, AO,
shadows, original scene triangles and full-rate glossy estimators remain intact.
Both Forward+ and deferred use identical GI settings in their comparison runs.
The upstream SurfelPlus source subset and its licensing files are unchanged.

The grid allocates at most 27 indices per surfel, 262,144 cell headers, 4,096
prefix sums and 262,144 allocation claims. This increases bounded grid storage
by approximately 28.8 MiB at the maximum 262,144-slot pool. Actual list occupancy
is much smaller; there is no GPU readback in normal rendering or timing runs.

## Compiled

MSVC Windows x86_64 Mono production/speed editor and release export template.
All 23 native compute variants pass glslang and spirv-val for Vulkan 1.2;
imported SurfelPlus shader hashes pass unchanged. The Sponza release embeds its PCK.

## Measurement and validation

Final measured values, binary hashes and separate validation outcomes are recorded
in [gi-optimization-2026-09-17.json](gi-optimization-2026-09-17.json).
Raw frame arrays, logs, GPU readbacks and screenshots are kept under
`%TEMP%/kiln-gi-optimization/`.

Whole-frame timing uses VSync off, 1920x1080, TAA, sun/sky, wet floor roughness
0.18, diffuse quality 2 and two rays per sampled reflection pixel. Each short run
has 480 frames: discard 180, measure 300. No validation layers, GPU profiling or
readbacks are enabled in these runs. A longer deterministic camera/TOD sequence
also checks behavior beyond the initial view. Pass profiling and functional
captures are separate experiments. GI-off keeps AO and GI resolve housekeeping.

## Whole-frame results

| Run | Mean ms | P95 ms | FPS |
| --- | ---: | ---: | ---: |
| Deferred, full GI | 6.307 | 6.523 | 158.55 |
| Deferred, diffuse only | 3.518 | 3.692 | 284.24 |
| Deferred, GI off | 2.201 | 2.374 | 454.35 |
| Forward+, full GI | 8.640 | 9.013 | 115.74 |
| Forward+, GI off | 1.925 | 2.084 | 519.41 |
| Moving deferred, full GI | 7.283 | 8.679 | 137.31 |
| Moving deferred, diffuse only | 4.352 | 4.950 | 229.79 |
| Moving deferred, GI off | 2.197 | 2.379 | 455.23 |
| Deferred, full GI repeat | 6.380 | 6.654 | 156.73 |
| Forward+, full GI repeat | 8.690 | 9.032 | 115.07 |
| Deferred, full-rate reference | 7.040 | 8.161 | 142.05 |
| Extended moving, 3600 frames | 7.201 | 8.695 | 138.87 |

The original static full-GI release was 98.84 ms / 10.12 FPS. The first optimized
run is 15.7x faster end-to-end. Full-rate reference sampling is still
7.04 ms, so the main improvement comes from cache/index/allocation work.

The extended run advances 60 seconds of deterministic scene time, including a
complete TOD cycle and camera movement near geometry. After the same 180-frame
warmup it measures 3420 frames, maximum 10.246 ms, with
0 intervals above 16.67 ms. The intermediate version without allocation claims
had a 351.451 ms maximum on this path. That intermediate spike is fixed in the
final run. The initial 480-frame view alone does not exercise that failure.

These static and moving tests meet the 1080p/60 FPS target on this host. The
optional multi-light workload has functional coverage, but no matching long
performance acceptance is claimed here.

## GPU pass measurements

| Pass/group | Before ms | After ms |
| --- | ---: | ---: |
| Surfel allocation queries | 5.434 | 0.224 |
| Diffuse rays | 6.358 | 0.439 |
| Diffuse screen evaluation | 6.008 | 0.261 |
| Diffuse pipeline including cache/resolve | 18.097 | 1.378 |
| GGX reflection rays | 79.755 | 2.845 |
| Reflection filtering | 0.460 | 0.572 |
| Specular pipeline | 80.215 | 3.417 |
| GI pipeline total (excluding AO) | 98.311 | 4.795 |

The pass values average the last two printed GPU-profile intervals. The engine
omits printed stages below 0.01 ms, so grouped sums are approximate. The diffuse
group includes update, both grid builds, generation, tracing, integration when
printed, screen evaluation and publish. Specular includes tracing and filtering.
Profiler runs are separate from the uninstrumented whole-frame table; GI-off
also retains filter/resolve housekeeping, so a toggle difference is not the
complete GI-pass duration.

## Real-GPU verification

- Editor, 960x540: 105 diffuse/G-buffer checks, 276 lighting/motion checks,
  223 reflection checks, 25 TOD checks, geometry import and resource lifecycle.
- Embedded release, 1920x1080: 223 reflection checks, including roughness/F0/
  metallic response, offscreen reflection, motion, light turn-off and odd resize.
- Full-rate reference, 960x540: all 223 reflection checks pass independently.
- Surfel debug views: 83 checks against actual cache entries pass.
- Four GPU grid captures (including resize, moving geometry and software BVH):
  2048 brute-force spatial queries each, with no missing candidates, invalid
  offsets, dead entries or duplicate bucket entries.
- Forced compute BVH, 480x270: 160 windowed frames, 10 diffuse checks and finite,
  nonzero specular. Forward+ and deferred, 960x540: same camera/TAA/ray settings,
  valid diffuse/specular, display-RGB MAE 0.003380 (descriptive).
- Runtime and validation-layer logs checked clean; hardware/software query
  diagnostics report zero mismatches. Actual screenshots were inspected for
  wet Sponza, sharp/rough emission, offscreen reflection, software BVH, Forward+
  and the cache disk view.

Default/full-rate display-RGB MAE across the nine recorded views is 0.001081–0.003157.
This includes stochastic cache/ray variation and is not a universal quality bound.
Release stationary display noise is 0.001626 (limit 0.005);
motion-settled/reference MAE is 0.001390 (limit 0.035).

## Reproduce

```powershell
python kiln/tools/check_surfel_shaders.py
python kiln/tools/validate_sponza.py --engine ./bin/godot.windows.editor.x86_64.mono.console.exe --size 960x540 --output "$env:TEMP/kiln-gi-acceptance"
python kiln/tools/benchmark_sponza.py --engine ./bin/godot.windows.editor.x86_64.mono.console.exe --output "$env:TEMP/kiln-gi-timing"
```

For an exported executable, use `--engine <exported-exe> --embedded`. Add
`--full-specular-rate` to the benchmark runner for full-rate reference runs.
For individual passes, run the engine with `--gpu-profile` before `--`, followed
by `--benchmark --still --hardware --size=1920x1080 --frames=480` and an output
directory. Do not mix profiler runs with the uninstrumented timing table.

Grid verification accepts one captured directory, for example:
`python kiln/tools/check_surfel_grid.py <capture>/comparison/multibounce`.
It checks prefix offsets, bounds, duplicate indices, live slots, and candidate
completeness against brute-force spatial queries on actual GPU cache data.

## Scope and remaining limits

This is still the engine's diffuse Surfel cache plus independent GGX reflection
path. It does not add SurfelPlus's directional radiance atlas, RIS or ray guiding.
Existing [transport/platform limitations](SURFEL-GI.md#unsupported-or-unverified)
remain. Other GPUs, APIs and operating systems are not verified by these results.
The two-ray reflection estimator retains Monte Carlo grain; reconstruction does
not make it noise-free. Short benchmark results are not a thermal endurance claim.
