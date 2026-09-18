# Diffuse relighting and viewport resize

Follow-up to the [cold-cache convergence revision](DIFFUSE-CONVERGENCE-2026-09-18.md).
This fixes two distinct causes of the reported flashing: global raw-sample
replacement during light revisions, and destruction of the world-space cache
when the viewport is reconfigured.

## Reference review

The [2021 GIBS presentation](https://advances.realtimerendering.com/s2021/SIGGRAPH%20Advances%202021%20-%20Surfel%20GI.pdf),
PDF pages 74–79, describes short/long means and variance controlling convergence
and ray requests. Page 116 places irradiance sharing before MSME. The estimator
and sharing diagrams and their speaker notes were inspected again for this fix.
A light revision is not a reason to replace every converged surfel with a noisy
new estimate.

[W298/SurfelGI](https://github.com/W298/SurfelGI/tree/8361942f7d799632b32d37356b8057814456a8a2)
uses MSME after irradiance sharing in `SurfelIntegratePass.cs.slang`.
`SurfelGI::execute()` separates resolution-dependent and independent resource
creation; the surfel buffers are independent of the frame dimensions.
`SurfelEvaluationPass.cs.slang` reduces new surfels' contribution and normalizes
RGB by the same weight. Those are the relevant contrasts with Kiln, rather than
a claim that its runtime has zero noise or zero latency. Its defaults, ray budget
and visibility implementation differ. Its Falcor/DX12 runtime was not run here.
No W298 code was copied. Existing imported MSME source and licenses are unchanged.

## Implemented

- Viewport cleanup now releases screen textures and NRD history independently.
  Surfel estimates, progressive sample indices, guiding distributions, recent
  batches and hardware acceleration structures survive resize. Growing the pool
  copies persistent buffer prefixes and clears only new entries; shrinking keeps
  the existing capacity. Environment replacement and explicit GI/history resets
  still invalidate transport. Screen reprojection and AO histories restart.
- Removed the global raw-batch blend that overrode MSME for every light revision.
  It previously admitted up to half a new shared batch per update, continuously
  under TOD, regardless of whether a receiver's lighting changed appreciably.
- Relighting shares fresh neighboring batches, avoiding recursively feeding old
  integrated means into a second temporal estimator. Static sharing is retained.
- Measured changes in sun direction/radiance and sky radiance distinguish gradual
  TOD from abrupt changes. Geometry, material and local-light edits request the
  stronger response. A decaying response controls the short-mean blend from 0.08
  to 0.4; the fresh sampling window lasts 64 frames. This is a Kiln adaptation,
  not a claim to reproduce Frostbite or W298 exactly.
- Catch-up targets the firefly-filtered short mean, using local short/long mean
  disagreement and an approximate EMA error scale. The relative uncertainty cap
  prevents the imported absolute variance floor from freezing almost-black light.
  Staggered/sleeping updates account for elapsed frames, so offscreen transport
  does not retain old multibounce light after visible receivers have responded.
  Neighbor correlation means this error scale is a heuristic, not a statistical
  confidence guarantee.
- Newly sampled surfels acquire gather influence over the configured convergence
  sample count (default 256, formerly 64). The same weight divides RGB; this is
  not an energy fade. Mature neighbors therefore dominate newly added coverage.
- Added a deterministic real-window relighting test and raw-readback comparison.
  Metadata records response strength, relighting duration and screen-history
  validity. The previous material lifecycle test now waits out the 64-frame window.

The default 196,608 primary-ray ceiling, light energies, scene geometry, materials,
surfel footprint and screen reconstruction kernel are unchanged. Secondary rays
are still outside that primary-ray counter. No performance improvement is claimed.

## Compiled

macOS arm64 Metal editor and release template. Native GLSL/SPIR-V variants and
imported-source hashes pass. This is not a Windows/Vulkan runtime validation.

## Real GPU results

Apple M5, Metal hardware ray queries, real windows, deterministic 60 Hz scene
steps; sun/sky diffuse, multibounce, NRD and indirect specular off. Mode 25 also
disables TAA. Compare the saved pre-change binary with the rebuilt editor.
Captures and logs are under `/tmp/kiln-relight-20260918/`.

The same interior floor mask and sigma-16-at-1080p luminance high-pass filter as
the earlier reviews are used. Temporal fluctuation is the standard deviation of
high-pass **consecutive-frame differences**, divided by mean light over sixteen
frames. It differs from the earlier stationary temporal-standard-deviation metric.
It can include real moving shadow detail and is not path-traced error. Spatial
residual includes genuine gradients. Both operate on `raw.bin`, before material,
AO, screen reconstruction and final TAA.

| 1920×1080 run / metric | Before | After |
| --- | ---: | ---: |
| Static temporal fluctuation | 0.05061% | 0.05054% |
| Continuous TOD temporal fluctuation | 3.6265% | 0.7832% |
| TOD endpoint spatial residual | 3.9237% | 1.8483% |
| Abrupt TOD step, 32-frame residual | 5.0069% | 3.9278% |
| Resize to 641×361, first-frame residual | 33.3818% | 0.9105% |
| Resize to 1280×720, first-frame residual | 44.1325% | 0.9273% |

Continuous-TOD fluctuation falls **78.4%**. Both resizes retain **100%** of the
previously alive surfels, including anchors and nondecreasing accumulated ray
counts/progressive indices; frame counters continue instead of restarting at zero.
The 32-frame abrupt-step mean is within 5% of the old late-frame result. The
combined regression passes 319 setup, budget, hardware-query, response and resize
checks. Compact measurements, checks and delivery hashes are recorded in
[diffuse-relighting-validation.json](diffuse-relighting-validation.json).
Full-field and fixed-scale floor comparisons were visually inspected. Abrupt
steps still have residual mottling; this is not a claim to eliminate all noise.

The separate original 1080p response suite passes all 18 checks plus the material
edit/restoration assertions. Light-off mean is 0.00988% of day after 32 frames and
0.000247% after 128; restored light at 128 is within 0.67% of day. The debug output
also matches the raw linear values at the declared gain.

A separate full 2,700-frame noon-to-noon trajectory completes, with five captures
passing fifteen frame/budget/query checks. A 960×540-to-1280×720 pool-growth test
retains cache frame continuity, explicitly resets history, then verifies another
resize with TAA and actual GGX reflections enabled. The release template renders
the existing self-contained Sponza pack in a real Metal window: deferred mode 25
at 1280×720 and a Forward+ GI-off smoke check at 960×540. Captures were inspected.
The latter is a runtime smoke check, not an equal-quality renderer comparison.

Cold startup remains comparable after 32–64 frames, but its first 8–16 frames
regress with the longer new-surfel influence ramp. The same-run before/after
raw spatial residuals are 5.63%/6.79% at frame 8, 3.30%/4.41% at frame 16,
2.20%/2.28% at frame 32, 1.020%/1.034% at frame 64 and 0.641%/0.624% at frame 512.
The startup lifecycle/setup checks pass; they do not assert a quality improvement.
This tradeoff must not be reported as faster cold convergence.

## Limits

Finite response lag remains. A matching 1920×1080 reference held at the continuous
TOD endpoint for 512 frames measures 0.027341. The moving-TOD mean is 0.029859,
about 9.2% higher during this rapid 45-second day; before the fix it was 0.5% below
the fixed-phase reference. Thus quieter continuous lighting has a measurable
response-lag tradeoff, despite passing the separate abrupt-change tests.
Readbacks stall rendering:
frame counts must not be presented as measured wall-clock convergence or FPS.
No fair Forward+ timing comparison or platform performance target was attempted.
Local/emissive workloads and depth-moment visibility remain outside this quality
claim; the latter is still unsupported. The tests do not establish parity with
the reference runtime. Screen-space reflection history still restarts on resize;
the shaded first frame can show reflection noise, outside the isolated diffuse
quality measurements above.

NRD is currently compiled only for Windows/Vulkan in this repository. Its strict
activation test fails on this Mac because NRD is unavailable; it was stopped and
is **not** counted as a passing NRD lifecycle validation. NRD resource recreation
and Windows/Vulkan runtime remain unverified by this revision.

## Reproduce

```sh
bin/godot.macos.editor.arm64 --path kiln/sponza --rendering-driver metal \
  --rendering-method kiln_deferred --gpu-validation --fixed-fps 60 \
  --script res://tests/diffuse_relighting.gd -- --still --no-specular \
  --size=1920x1080 --output=/tmp/relighting-after

python3 kiln/tools/compare_diffuse_relighting.py \
  /tmp/relighting-before /tmp/relighting-after \
  --output /tmp/relighting-comparison --plot
```

Run the same scene with the saved before binary for the first directory. NumPy
is required; plotting also uses matplotlib. `--tod-cycle` selects a full 2,700-frame
noon-to-noon trajectory. `--tod-reference` instead holds the endpoint phase for
512 frames to measure remaining response lag.
