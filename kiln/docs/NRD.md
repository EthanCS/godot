# NRD indirect lighting

**Latest: [full-rate reflections and roughness review](NRD-REFLECTIONS-2026-09-18.md).**
Deferred's primary G-buffer now enables DFG-based material demodulation and a
single RELAX_SPECULAR signal. Full-rate two-ray specular is the normal NRD mode;
checkerboarding is opt-in after measured brightness and recovery differences.
Forward+ retains two Schlick signals because its prepass lacks primary material
data. The diffuse contract below is unchanged. Reflection measurements in the
new report supersede the earlier unvalidated-reflection scope statement.

**Previous diffuse validation: [cache diffuse + RELAX](NRD-CACHE-DIFFUSE-2026-09-18.md).**
Diffuse always comes from the surfel cache. The adapter declares full-rate
`RELAX_DIFFUSE`, plus the optional specular signals described above when
reflections are enabled. Diffuse preblur and hit-distance reconstruction are off:
the pinned RELAX implementation does not consume diffuse hit distance in this
configuration. Its unused alpha is zero; no distance or screen rays are invented.
Toggling NRD does not clear the diffuse cache. NRD replaces the original diffuse
screen/temporal filter. The new report separates actual improvements from
regressions; it does not claim that cache blotches have been solved.
That GPU test covers sun/sky diffuse. Reflection, local/emissive and
non-Windows/non-Vulkan quality were not revalidated by that test.

The implementation description and results below document the **previous
diffuse-plus-specular NRD revision**, retained as historical evidence. They do
not validate the current adapter or describe current diffuse
transport.

Kiln integrates **NVIDIA NRD v4.17.3**, release commit
`792eff196afdd350fd9c3f862119017ccb438a0e`.
The SDK is an optional external dependency under the NVIDIA RTX SDKs LICENSE,
not the engine's MIT license. No NRD SDK source is vendored into this repository.

## Implemented

- Native RenderingDevice adapter for NRD compute pipelines, SPIR-V bindings,
  permanent/transient texture pools, constants, samplers and per-viewport history.
- RELAX diffuse/specular plus a separate RELAX specular history for the second
  Schlick integral. Primary albedo, metallic, F0/F90 and engine BRDF compensation
  remain in the existing material composition. These material-independent basis
  signals are not the final shaded screen image.
- Two fresh cosine-distributed diffuse rays per pixel, with explicit emitter
  sampling and real first-hit distance. Surfel GI supplies secondary illumination
  and multibounce; the primary diffuse image no longer directly displays the
  sparse cache gather. Ray-hit texture/geometry support retains existing limits.
- Full-rate GGX specular inputs, positive minimum hit distance across valid rays,
  world normals, linear roughness, positive linear view depth, non-jittered motion
  and separate jitter. Diffuse has a longer history; changing illumination uses
  a shorter history. Invalid/sky output is cleared and negative filter undershoot
  is clamped before material composition.
- Deferred uses rasterized object motion. Forward+ uses camera reprojection,
  removes jitter from that motion, and resets NRD on dynamic geometry changes;
  its dynamic-object history quality is not equivalent to deferred object motion.
- NRD owns the active reflection filter; the previous reflection filter runs only
  for fallback. AO and scene-geometry history remain independent.
- SDK version/normal encoding are checked. The Windows build pins the SDK commit.
  NRD's optimized DXC SPIR-V bypasses re-spirv only for the named NRD pipelines:
  that optimizer produced invalid entry-point interface references in testing.
  The input modules are validated separately with spirv-val.

## Build and use

On Windows with Visual Studio 2022, CMake and the normal engine build dependencies:

```powershell
python kiln/tools/prepare_nrd.py
# Then run the normal engine editor/template SCons builds.
```

The SDK is fetched into ignored `bin/build_deps/NRD`. `kiln_nrd_sdk=<path>` selects
another pinned SDK checkout; `kiln_nrd=no` builds without it. Its license and
dependency notices remain in the SDK directory. Build scripts never publish it.

`rendering/kiln/nrd=true` enables NRD when the compiled SDK and Vulkan are present.
NRD defaults to **off**; use `kiln/sponza/run.ps1 -NRD -Still` to opt in.
`-NoNRD` (CLI `--no-nrd`) remains an explicit override. Captures and
runtime statistics report actual `nrd_active`, version and diffuse ray count;
requesting NRD does not imply it is available on a different build/backend.

## Validation

Compiled: Windows x86_64 Mono production editor and release export template,
MSVC speed builds, pinned NRD static library and embedded SPIR-V. All native GI
shader variants pass glslang/spirv-val. The 18 dispatched NRD shader modules
dumped during bring-up pass spirv-val after entry-point adaptation.

Real GPU: Windows / NVIDIA GeForce RTX 5070 Ti / Vulkan 1.4.341, 2026-09-17.
At 960x540 the final editor passes 115 diffuse/G-buffer checks, 300 lighting and
motion checks, 223 reflection checks and 25 time-of-day checks, plus asset
import/serialization and resource lifecycle. A dedicated NRD test passes
on/off/on switching, camera-cut reset and 641x361 recreation. Accepted runs have
no engine/script or Vulkan validation errors. NRD activation is checked through
metadata/statistics, not inferred from a successful application startup.

Additional real-window checks exercise compute BVH at 320x180, Forward+ at
960x540 and the embedded release at 1920x1080. Visual review covers the
diffuse-only before/after images, wet reflections, rough red-emitter reflections,
blue offscreen-emitter reflections, release, Forward+ and compute-BVH output.
Captures/logs live in `%TEMP%/kiln-nrd/`; source/binary hashes and compact results
are in [nrd-validation.json](nrd-validation.json).

### Quality and the failed spatial gate

The old and new stationary 960x540 runs use the same camera and lighting:

| Measurement | Previous cache/filter | NRD |
| --- | ---: | ---: |
| Floor mean diffuse light | 0.043461 | 0.040820 |
| Relative floor spatial residual | 0.022768 | 0.021619 |
| Stationary display RGB temporal standard deviation | 0.002054 | 0.001773 |
| Reflection-suite display RGB temporal standard deviation | 0.001987 | 0.001654 |
| Reflection motion settled/reference MAE | 0.001238 | 0.000586 |

The fixed floor patch's spatial residual improves **5.0%**, so the existing
**35% spatial-improvement gate fails**; its energy and temporal-noise gates pass.
The threshold was not weakened. Overall stationary display variation falls
13.6%; the separate reflection-suite variation falls 16.8%. These display metrics
include TAA and other image variation, and the high-pass spatial residual includes
real lighting gradients. They are not error against a path-traced reference.
Diffuse primary transport changed as well as its denoiser: these comparisons
measure the complete new path, not NRD in isolation. Visually, broad cache
mottling is reduced, but fine grain and residual variation remain. This is not
a claim that the GI noise problem is completely solved.

### Performance

Embedded release, 1920x1080, TAA, two screen diffuse rays and two full-rate
specular rays. Timing excludes readbacks and validation layers. Short runs use
180 warmup + 300 measured frames; the moving run uses 180 + 1020 frames.

| Run | Mean ms | P95 ms | Average FPS |
| --- | ---: | ---: | ---: |
| Deferred full GI | 16.687 | 17.995 | 59.93 |
| Deferred full GI repeat | 14.571 | 15.816 | 68.63 |
| Forward+ full GI | 17.377 | 18.425 | 57.55 |
| Forward+ full GI repeat | 17.481 | 21.259 | 57.21 |
| Extended moving deferred | 15.488 | 17.688 | 64.57 |

Forward+ and deferred use the same NRD and ray settings for these static
comparisons. The extended moving run has 232/1020 frames over 16.67 ms and a
maximum of 18.942 ms; stable 60 FPS is not established. This higher-quality path
costs substantially more than the earlier cache-only diffuse reconstruction.
The initial four-diffuse-ray trial (about 53 FPS static) is retained under
`benchmark/` but is not the final configuration. Final timings are under
`final-benchmark/`.

Reproduce functional checks with `kiln/tools/validate_sponza.py`; use
`res://tests/nrd_lifecycle.gd` for NRD switching and
`res://tests/gi_noise.gd` with `kiln/tools/check_gi_noise.py` for paired noise
captures. `kiln/tools/benchmark_sponza.py` runs the sequential equal-quality
renderer comparison. All of these rendering checks run in real windows.

## Unsupported and distribution limits

The current adapter is enabled only for Windows/Vulkan builds. Metal, D3D12,
Linux and AMD/Intel GPUs have not been compiled/visually validated for this
integration. This is an implementation/verification limit, not a GPU-brand
license restriction. Other configurations retain the original GI/filter path.
Existing unsupported transport features in SURFEL-GI.md still apply. NRD cannot
repair omitted geometry, light leaks or missing transport paths.

NRD is separately licensed, with distribution, attribution/trademark and other
conditions; this integration does not relicense it as MIT or resolve permission
to redistribute its source. Before public distribution of a bundled build,
review the pinned SDK's LICENSE.txt and dependency notices and satisfy its
attribution/distribution requirements. No public publishing occurred here.

Official release: https://github.com/NVIDIA-RTX/NRD/releases/tag/v4.17.3
License: https://github.com/NVIDIA-RTX/NRD/blob/v4.17.3/LICENSE.txt
