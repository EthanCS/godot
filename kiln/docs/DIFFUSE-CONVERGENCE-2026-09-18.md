# Indirect diffuse cold-cache convergence

This follows the [diffuse-value review](DIFFUSE-VALUES-2026-09-18.md). Its
240/994-frame measurements did not validate startup latency. NRD remains off.

## What the references actually demonstrate

The [SIGGRAPH 2021 GIBS slides and speaker notes](https://advances.realtimerendering.com/s2021/SIGGRAPH%20Advances%202021%20-%20Surfel%20GI.pdf)
do describe slow cases. PDF pages 147–152 show approximately 700 visible lights
and another 700 outside the view: page 148 describes roughly five seconds to
acceptable indirect diffuse. A different light-sampling method approaches that
result after 15 frames, at greater cost. Pages 187–190 show a 1,400-visible-light
stress case still noisy after ten seconds, even with a million-ray budget.
These are many-light cases, not evidence that a sun/sky Sponza must take seconds.
Pages 118–122 show sharing greatly improving results after **64 samples**;
samples must not be presented as frames or a guaranteed wall-clock time.

[W298/SurfelGI](https://github.com/W298/SurfelGI/tree/8361942f7d799632b32d37356b8057814456a8a2)
also accumulates lighting. Its defaults request 16–64 rays per active surfel
per frame and 1–4 for sleeping surfels, subject to the global budget. The
`blendingDelay = 240` setting ramps a surfel's contribution, but RGB is divided
by the accumulated weight. It is not a mandatory 240-frame black interval or
proof of four-second convergence. Its 9,600,000-ray capacity also differs from
Kiln's 196,608 primary-ray ceiling. The repository's Sponza teaser is a composed
still image. No cold-start raw-diffuse timing was measured from its Falcor/DX12
runtime on this Mac, so equivalence or superiority is not claimed.

## Implemented

- Bootstrap accounts for the old-light fraction already present in shared
  irradiance. Previously, 75% reused light followed by a 1/32 blend admitted
  only 1/128 fresh input in the uniform-error case. This is an explanatory
  linear model, not a measured time constant for every surface.
- Bootstrap confidence now grows with actual accumulated rays, with a 1/256
  minimum fresh blend, instead of retaining a fixed 1/32 noise floor throughout
  the 2,048-ray bootstrap. The original MSME source is unchanged.
- An empty cache no longer enters the 32-frame relighting window. Real changes
  to lighting and materials retain that fast-response path.
- Geometry capture retains the material hash from the completed capture. The
  initial discovery scan uploads texture pages while computing its hash; keeping
  that intermediate hash caused an unchanged second frame to report a material
  change and restart the relighting window.
- Added early-frame GPU captures and a numeric comparison tool. All measurements
  below use `raw.bin`, before material, AO, screen reconstruction and final TAA.

The ray-budget ceiling, surfel footprint, spatial kernel and light energy are
unchanged. Actual initial ray counts differ: bypassing the relighting override
reduces the first frame from 186,288 to 93,144 rays in these runs.

## Compiled

macOS arm64 Metal editor and release template rebuilt successfully. All native
GI GLSL/SPIR-V variants and imported-source hash checks passed. Windows/Vulkan
runtime graphics were not tested.

## Real GPU validation

Apple M5, Metal hardware ray queries, real windows, 1920×1080, sun/sky,
multibounce, NRD/specular off. Mode 25 disables TAA. Two before/after runs use
the same camera and fixed scene timeline. Allocation and scheduling can vary
between GPU runs, so the table gives both runs' ranges. Captures and logs are
under `/tmp/kiln-convergence-20260918/`; compact results and binary hashes are
in [diffuse-convergence-validation.json](diffuse-convergence-validation.json).

The same interior floor mask (x=34–52%, y=80–96%) and sigma-16 luminance
high-pass residual / mean from the previous report are used. Real gradients
remain in this metric; it is not path-traced error. Frame labels count from 1;
capture metadata counts from 0.

| Rendered frames | Before residual | After residual |
| --- | ---: | ---: |
| 8 | 6.13–6.25% | 6.34–6.39% |
| 16 | 5.59–6.32% | 3.77–4.04% |
| 32 | 5.34–5.46% | 1.98–2.37% |
| 64 | 2.63–2.89% | 0.99–1.01% |
| 128 | 1.10–1.27% | 0.69–0.73% |
| 240 | 0.708–0.715% | 0.65–0.66% |
| 512 | 0.64–0.66% | 0.64–0.66% |

At 32 frames the residual falls 56–64%; at 64 frames it falls 62–65%.
The new 64-frame output is slightly smoother than the old 128-frame output;
the new 128-frame output is approximately as smooth as the old 240-frame output.
This supports roughly halving the frame count for those observed quality
levels, not claiming instant convergence. Fixed-scale full-field and floor
plots were visually inspected. At frame 512 the two runs' floor mean changes
are approximately +0.19% and −0.02%.

Sixteen consecutive raw captures after frame 512 measure temporal standard
deviation / mean of 0.02935% before and 0.02785% after in the repeated pair.
The repeat passes 79 setup, frame, budget and lifecycle checks. Static startup
keeps material revision 1 and never enters relighting. The separate real-GPU
response suite passes 18 value checks plus six material-update assertions:
light-off reaches 0.0209% of day after 32 frames, restored light is within
0.66% at 128 frames, the debug texture matches raw times gain, and actual
material edits/restoration still activate relighting and then settle.

The rebuilt release template also runs the existing self-contained Sponza pack
in a real Metal window: deferred mode 25 at 1280×720 and a Forward+ GI-off smoke
check at 960×540. Both captures were inspected and logs are clean. This is a
runtime smoke check, not an equal-quality renderer performance comparison.

Readback stalls make these runs unsuitable for measuring wall-clock convergence
or FPS. At an actual 60 FPS, 64 frames would take about 1.07 seconds; this is a
conversion, not a measured frame-rate result.

## Remaining limits

The first 1–8 frames remain noisy; the first frame is worse with fewer rays.
Small undulations and corner artifacts remain. The measurements cover one
stationary sun/sky camera, not every disocclusion, animated object or light
configuration. No claim is made that Kiln now matches the reference runtime,
that its transport is unbiased, or that other platforms meet a performance
target. The earlier missing depth-moment visibility remains unsupported.

## Reproduce

Run the same script with the saved before binary and rebuilt after binary:

```sh
bin/godot.macos.editor.arm64 --path kiln/sponza --rendering-driver metal \
  --rendering-method kiln_deferred --gpu-validation --fixed-fps 60 \
  --script res://tests/diffuse_startup.gd -- --still --no-specular \
  --capture-temporal --size=1920x1080 --output=/tmp/diffuse-startup-after

python3 kiln/tools/compare_diffuse_startup.py \
  /tmp/diffuse-startup-before /tmp/diffuse-startup-after \
  --output /tmp/diffuse-startup-comparison --plot
```

The comparison requires NumPy; plotting additionally requires matplotlib.
The light-response reproduction command remains in the preceding report.
