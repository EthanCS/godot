# Adaptive surfel-ray budget — 2026-09-18

## Scope and baseline

The current fixed 2,097,152 primary-ray ceiling is used as the visual baseline.
It is a correctness-first renderer result, not a path-traced ground truth. Two
independent fixed-ceiling runs establish the renderer's own allocation and
sampling noise before lower budgets are compared.

All image comparisons below use the deterministic self-contained Sponza scene,
sun/sky diffuse, multibounce, NRD off and indirect specular off. Performance and
readback captures are separate runs. The captures remain under
`/tmp/kiln-adaptive-budget-20260918`; readback runs are not timing results.

## Implemented

- The configured 2,097,152-ray value remains the correctness ceiling.
- Default adaptive tiers are 393,216 rays while stationary, 524,288 during
  camera motion or continuous lighting changes, and 1,048,576 for the first 16
  cache frames.
- An abrupt lighting/material/dynamic-geometry response ramps smoothly from the
  motion tier toward the configured ceiling. It decays with the same measured
  lighting response used by the estimator rather than a new fixed timer.
- A configured ceiling smaller than a tier remains a hard cap. Zero retains its
  historical meaning of unrestricted rays. Adaptive budgeting can be disabled.
- Captures and public GI statistics report the effective budget, configured
  ceiling, adaptive state and active mode (`bootstrap`, `stationary`, `motion`,
  `lighting`, `relighting`, `configured_cap`, `fixed` or `uncapped`).
- An explicit Sponza `--surfel-ray-budget=<N>` run remains a fixed-budget A/B.
  `--adaptive-budget` can opt it back into adaptive behavior.

Project settings:

| Setting | Default |
| --- | ---: |
| `rendering/kiln/surfel_ray_budget` | 2,097,152 ceiling |
| `rendering/kiln/surfel_adaptive_budget` | enabled |
| `rendering/kiln/surfel_stationary_ray_budget` | 393,216 |
| `rendering/kiln/surfel_motion_ray_budget` | 524,288 |
| `rendering/kiln/surfel_bootstrap_ray_budget` | 1,048,576 |

## Real-GPU performance

Apple M5 (`Apple9`), Metal 4.0 hardware ray queries, 1920×1080 editor build,
diffuse-only. Each row uses 180 warm-up frames and 120 measured frames. These
editor results are an A/B for this change and are not release-build targets.

| Deterministic workload | Fixed 2,097,152 | Adaptive | Change |
| --- | ---: | ---: | ---: |
| Static mean frame time | 50.030 ms | 33.472 ms | -33.1% |
| Static FPS from mean | 19.99 | 29.88 | +49.5% |
| Moving/TOD mean frame time | 41.023 ms | 37.428 ms | -8.8% |
| Moving/TOD FPS from mean | 24.38 | 26.72 | +9.6% |

The final static frame selected the 393,216 stationary tier. The moving run
selected the 524,288 lighting tier. Fixed-budget sweeps measured 33.950 ms at
393,216, 36.830 ms at 524,288 and 43.945 ms at 786,432 for the static case.

Keeping the default two-ray indirect reflections enabled and NRD off gives the
following whole-frame result. Reflection settings are identical on both sides;
this table measures the total benefit without claiming a reflection-quality
change.

| Full GI workload | Fixed 2,097,152 | Adaptive | Change |
| --- | ---: | ---: | ---: |
| Static mean frame time | 67.114 ms | 50.544 ms | -24.7% |
| Static FPS from mean | 14.90 | 19.78 | +32.8% |
| Moving/TOD mean frame time | 58.163 ms | 54.610 ms | -6.1% |
| Moving/TOD FPS from mean | 17.19 | 18.31 | +6.5% |

## Real-GPU image comparison

At 960×540, 120-frame continuous-TOD and 120-frame camera-motion sequences were
captured for two fixed-ceiling baselines and the adaptive result:

| Displayed sequence metric | Fixed baseline A | Fixed baseline B | Adaptive |
| --- | ---: | ---: | ---: |
| TOD temporal high-pass std. dev. | 0.00079837 | 0.00079765 | 0.00079963 |
| Motion temporal high-pass std. dev. | 0.0624635 | 0.0625053 | 0.0624727 |

The adaptive result is within the two baseline runs' spread for the
parallax-dominated motion metric and differs by 0.25% from their mean for TOD.
Representative TOD/motion frames and amplified-difference contact sheets were
visually inspected; no new disk flash or budget-transition discontinuity was
observed. The numeric stability analysis covers all 240 saved frames.

At 1920×1080, final composed sRGB images were compared directly to baseline A.
The baseline-B comparison measures the renderer's run-to-run floor:

| Workload | Baseline-B MAE / RMSE | Adaptive MAE / RMSE | Adaptive P99 abs. |
| --- | ---: | ---: | ---: |
| Static | 0.000320 / 0.001161 | 0.000397 / 0.001304 | 1/255 |
| Moving/TOD | 0.001083 / 0.003075 | 0.001300 / 0.003450 | 4/255 |

The adaptive diffuse energy ratios to baseline A are 1.00029 static and 1.00034
moving. Visually, the remaining amplified difference follows fine indirect-light
and temporal-detail regions rather than a broad energy or color shift.

The 960×540 cold-cache floor energy ratio to the fixed ceiling is 0.982 at frame
4, 0.996 at frame 8, 1.003 at frame 16 and within 0.1% from frame 32 through 512.
Actual fixed-ceiling requests in the first 16 frames remain below the adaptive
1,048,576 bootstrap tier after the initial empty frame.

## Compiled and checked

- `python3 kiln/tools/check_surfel_shaders.py`: all native and translated shader
  variants and imported-source hashes passed.
- `bash kiln/tools/build_macos.sh --templates`: arm64 Metal editor and release
  template compiled successfully.
- `res://tests/adaptive_surfel_budget.gd`: bootstrap, stationary, motion,
  relighting, configured-cap, fixed and uncapped states passed in a real window
  with GPU validation.
- `res://tests/diffuse_stability.gd` and
  `kiln/tools/check_diffuse_stability.py`: adaptive 960×540 sequence passed and
  was visually inspected.

## Unsupported or not verified

- Windows/Vulkan, Linux, D3D12, Compatibility/Mobile and AMD/Intel GPUs were not
  run for this revision.
- Image comparisons hold NRD, indirect specular, local lights and emissive
  workloads off to isolate the diffuse primary-ray policy. A separate equal-
  settings default-reflection timing is reported, but reflection image quality,
  NRD, local/emissive workloads and their interactions were not revalidated.
- No release-build FPS, 4K result or platform performance target is claimed.
- The existing light-off multibounce tail still fails the old
  `off32_below_1pct` and `off128_below_0_1pct` assertions. Fixed 2,097,152 and
  adaptive runs fail by nearly identical values at both 960×540 and 1920×1080,
  so it is not counted as an adaptive-budget pass or regression.
