# Diffuse GI temporal stability revision — 2026-09-18

## Scope and reference

This revision addresses two visible failures in the persistent diffuse cache:

- continuous or stepped lighting first produced full-amplitude surfel noise,
  abruptly changed behavior after a fixed interval, then converged slowly;
- camera motion exposed newborn surfels using their first undersampled batch.

The implementation was compared against W298/SurfelGI commit
`8361942f7d799632b32d37356b8057814456a8a2` and the SIGGRAPH 2021 GIBS course.
It does not use another renderer's visible output as the acceptance standard.

## Implemented

- Replaced the binary 64-frame relighting state with a continuous response based
  on measured sun, sky and local-light change. Continuous TOD revisions no longer
  restart a global estimator mode every frame.
- Kept integrated neighbor history during a light transition and changed MSME
  short-window/catch-up bandwidth continuously. Local MSME inconsistency can
  finish a coherent transition after the global impulse decays.
- Faded stale ray guiding toward a cosine-uniform proposal during lighting
  changes and continuously damped only stale multibounce cache feedback. Direct
  light, environment and emission remain fully sampled.
- Changed the cache order to update, trace, share, integrate and evaluate existing
  surfels before generating new ones. A surfel allocated this frame cannot enter
  the displayed cache until the following frame.
- Initialized newborn radiance, long mean and short mean from already admitted
  neighboring cache entries, with a bounded 16-sample prior.
- Added a 16–64 sample admission confidence and propagated it through the two
  geometric reconstruction passes. Mature neighbors receive more reconstruction
  weight than unresolved newborn entries.
- Corrected global ray allocation: variance/bootstrap weights now represent the
  actual requested rays instead of being normalized back to the nominal total.
  Age-one surfels receive 32 rays when budget is available.
- Raised the correctness-first primary surfel-ray budget default from 196,608 to
  2,097,152. This is a quality setting, not a measured performance target.
- Added `res://tests/diffuse_stability.gd` and
  `kiln/tools/check_diffuse_stability.py` for deterministic 120-frame TOD and
  moving-camera sequences without capture readbacks changing frame cadence.

## Compiled and checked

- `python3 kiln/tools/check_surfel_shaders.py`: all native and translated shader
  variants passed, including imported-source hash checks.
- `bash kiln/tools/build_macos.sh`: arm64 Metal editor compiled successfully.
  Build log: `/tmp/kiln-build/editor.log`.
- `git merge-base --is-ancestor ed1daf0bf001b61586d9930840f2f1394092c079 HEAD`
  remained part of the build script and passed.

## Real-GPU visual validation

Hardware: Apple M5 (`Apple9`), Metal 4.0, real visible windows with GPU validation.
No headless run is counted as visual validation.

### Continuous TOD and moving camera

Command:

```sh
bin/godot.macos.editor.arm64 --path kiln/sponza \
  --rendering-driver metal --rendering-method kiln_deferred --gpu-validation \
  --script res://tests/diffuse_stability.gd -- \
  --still --no-specular --size=480x270 \
  --output=/tmp/kiln-diffuse-stability-final-20260918
python3 kiln/tools/check_diffuse_stability.py \
  /tmp/kiln-diffuse-stability-final-20260918
```

- All captured age-one surfels in the moving sequence received exactly 32 rays.
  New allocations remained excluded on their birth frame.
- Continuous TOD's maximum lighting response was `0.00824`; it did not enter a
  global high-response mode.
- The 8-bit displayed TOD sequence measured temporal high-pass standard deviation
  `0.000814`. This value is descriptive and includes quantization.
- The complete 120-frame TOD and moving-camera contact sheets were visually
  inspected. No full-amplitude newborn disks or fixed-duration flashing phase
  were observed.

### Relighting, resize and cache persistence

`res://tests/diffuse_relighting.gd` completed on the same GPU. The raw stationary
and continuous-TOD high-pass measurements were `0.000148` and `0.000387`.
After an abrupt TOD step, the inspected floor mean was within approximately
`3.3%` of the frame-256 value by frame 32, without a frame-64 behavior switch.
Resize retained `100%` of matching pre-existing surfels in the scripted check.

### Existing diffuse acceptance

`res://tests/diffuse_focus.gd` and `kiln/tools/check_diffuse_focus.py` passed:

- moving-shot geometric coverage: `99.716%–100%`;
- odd-size and restored-size coverage: greater than `99.999%`;
- stationary displayed temporal standard deviation: `0.00256`;
- the 240-to-944-frame floor residual decreased from `1.914%` to `1.737%`.

The existing surfel-debug suite also completed on Metal and
`kiln/tools/check_surfel_debug.py` passed all 84 checks. Its ray-update assertion
now accepts either visible temporal reuse or a valid full-rate visible set, since
the correctness-first budget can legitimately update every selected surfel.

Captures are in `/tmp/kiln-diffuse-stability-final-20260918`,
`/tmp/kiln-diffuse-relighting-fixed-v4-20260918` and
`/tmp/kiln-diffuse-focus-final-20260918`; debug captures are in
`/tmp/kiln-surfel-debug-final-20260918`.

## Unsupported or not verified

- No Windows/Vulkan, Linux or Compatibility/Mobile result was run for this
  revision. Existing renderers were not intentionally changed.
- No FPS, frame-time or 1080p performance target was measured. The larger default
  budget is deliberately correctness-first and is expected to cost performance.
- W298-style directional surfel depth moments are still not implemented. Current
  normal and plane rejection remains weaker around disconnected surfaces and
  corners.
- A sudden large viewport growth can expose one raw-cache frame with incomplete
  support because newborns are deliberately admitted on the following frame.
  The production reconstruction was visually smoother, but a teleport/resize
  stress sequence is not claimed to be mathematically flicker-free.
- With all authored sun/sky energy disabled, recursive cache energy decayed much
  faster than before but retained a small nonzero tail in this finite test. This
  is recorded rather than treated as a passed zero-energy proof.
