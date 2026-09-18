# Cache diffuse through NRD RELAX, 2026-09-18

NRD now processes the existing full-resolution surfel gather through
`RELAX_DIFFUSE`. It replaces the original two spatial passes and screen temporal
filter. The world cache, irradiance sharing, MSME, ray guiding, primary-ray budget,
miss continuation, lighting and composition are unchanged by this revision.

On RTX 5070 Ti at 1920x1080, the repeated production-release measurements add
**0.228 ms static / 0.267 ms moving** to the whole frame. Same-input measurements
show about **3% less floor spatial residual and 24% less stationary temporal
variation** with the normal TAA-enabled rendering configuration. These are modest
improvements, not a solution to persistent cache blotches. Some no-TAA results
regress; those results are included below.

## Implemented input contract

The SDK remains NRD 4.17.3 at commit
`792eff196afdd350fd9c3f862119017ccb438a0e`, an external dependency under its NVIDIA
license. No SDK source or license is replaced or vendored by this change.

- `IN_DIFF_RADIANCE_HITDIST.rgb`: the full-rate cache gather's irradiance / pi,
  before primary material factors, AO, exposure or tone mapping. The captured
  RGB is bit-identical to the cache input at valid geometry pixels.
- Diffuse prepass radius is zero, hit-distance reconstruction is off and diffuse
  checkerboarding is off. Alpha is unused and set to zero. This is specific to
  the pinned RELAX configuration, not a claim that arbitrary NRD denoisers can
  omit hit distance. In particular, REBLUR is not used for this signal.
- World-space normals and linear roughness use the SDK's rotated-octahedral
  R10G10B10A2 encoding. View Z is positive; sky is outside the denoising range.
  Deferred supplies rasterized, unjittered object/camera motion even without TAA.
  Projection matrices and jitter are supplied separately. Engine frame delta is
  supplied in milliseconds rather than depending on NRD's wall-clock timer.
- Diffuse history is capped at 8 frames (4 during relighting), fast history at 2,
  history repair at 1, A-trous iterations at 4. These are public RELAX controls.
  Geometry/luminance rejection and variance estimation retain SDK defaults.
  Additional firefly clipping is disabled: MSME already owns cache clipping.
- Optional reflections retain independent Schlick histories and actual hit
  distances. A diffuse-only instance allocates/dispatches only RELAX_DIFFUSE.
  Reflection quality is outside the current validation scope.
- Enabling/disabling NRD preserves the world cache and resets only denoiser
  history as needed. Resize recreates viewport resources. Unavailable/failed
  NRD falls back to the existing filter. Raw-cache debug bypasses both filters.

This corrects the earlier review's overly broad assumption that cache diffuse
could not be integrated without a meaningful per-pixel hit distance. The
[pinned SDK documentation](https://github.com/NVIDIA-RTX/NRD/blob/792eff196afdd350fd9c3f862119017ccb438a0e/README.md#combined-denoising-of-direct-and-indirect-lighting)
identifies RELAX's limited hit-distance use. Inspection of
[RELAX_PrePass](https://github.com/NVIDIA-RTX/NRD/blob/792eff196afdd350fd9c3f862119017ccb438a0e/Shaders/RELAX_PrePass.cs.hlsl)
and subsequent diffuse stages confirms that diffuse hit distance is unused with
this preblur/reconstruction configuration. No distance is fabricated and no
screen-pixel diffuse tracer is dispatched (`nrd_diffuse_rays == 0`).

## Quality and limitations

Diagnostic mode runs the original reconstruction alongside NRD against the exact
same frame's cache input, with separate post-filter histories. The reference is
the current GIBS revision with its two spatial passes and original temporal
filter, not the older, noisier cache implementation. Neither output feeds back
into the cache. The extra reference work is disabled in all timing runs.

The deterministic editor suite uses fixed 60 Hz simulation, isolated sun/sky
diffuse, no indirect specular, no local lights or emissive workload. Binary
signals are measured before materials, exposure and final TAA. With TAA enabled,
camera jitter still affects the input raster samples. Temporal statistics use
16 paired captures of a stationary floor patch, spaced two rendered frames apart.

Spatial residual is luminance minus a Gaussian low-pass, divided by patch mean;
sigma is 8 pixels at 960-wide, scaled with resolution. The floor mask is the
same geometric/roughness mask used in the previous diffuse review. It contains
real gradients as well as noise: this is **not error against a path-traced
reference**, nor proof that contact shadows or low-frequency bias are correct.

| 1080p, TAA enabled | Original filter | RELAX | Change |
| --- | ---: | ---: | ---: |
| Floor spatial residual at frame 240 | 0.0286034 | 0.0277460 | -3.00% |
| Converged floor spatial residual | 0.0192496 | 0.0186267 | -3.24% |
| Stationary floor temporal std / mean | 0.00183910 | 0.00140037 | -23.86% |
| Floor mean at frame 240 | 0.0441524 | 0.0440366 | -0.26% |
| Returned-camera floor spatial residual | 0.0277149 | 0.0268184 | -3.23% |
| Sunset floor spatial residual | 0.0573267 | 0.0484747 | -15.44% |

Sunset mean is 3.26% higher with RELAX, indicating additional temporal response
lag during continuous lighting changes; the entire spatial reduction must not
be interpreted as unbiased recovery. Resizing back to 1080p gives only 1.17%
less residual after the short warmup. Minimum measured cache coverage across the
1080p lifecycle is 99.9876%; NRD does not create that coverage.

The separate **960x540 no-TAA** suite is a counterexample to universal improvement:
frame-240 spatial residual falls 2.22%, but converged residual increases 2.00%,
and temporal std / mean increases from 0.00153601 to 0.00190683 (+24.14%). Mean
brightness changes by approximately -0.3%. These comparisons are within each
run's identical inputs; the no-TAA run is not compared against another run's
cache realization. Persistent cache error is correlated and is not the same as
independent pixel Monte Carlo noise. RELAX cannot be expected to remove it all.

Visual review covers the full indirect image, floor, curtain/column boundaries,
motion views, sunset, the 1080p exported lit image and a Forward+ smoke image.
Geometry boundaries remain visible, but broad floor/cloud patterns remain too.
The earlier failed 35% historical spatial-improvement gate remains unpassed;
the new input/lifecycle checker does not replace it with an easier quality gate.

## Performance

Windows x86_64 Mono production release, RTX 5070 Ti, Vulkan, full 1920x1080,
TAA, sun/sky diffuse only. Each case runs 1,200 frames: 180 warmup and 1,020
measured. No readbacks, reference filter, validation layers, profiling, build or
other rendering process runs during these timings. Opposite repeat order helps
expose run-to-run variation. The camera/sunlight timeline advances per frame.
Ray budget remains 196,608 primary surfel rays, with the same variance weighting,
secondary continuation and shadow work. Render counters confirm active windows.

| Case | Mean ms | P95 ms |
| --- | ---: | ---: |
| Static original filter | 4.8132 | 5.2281 |
| Static RELAX | 5.1154 | 5.5901 |
| Moving original filter | 5.8526 | 6.5842 |
| Moving RELAX | 6.0748 | 6.8761 |
| Static RELAX repeat | 5.1153 | 5.6151 |
| Static original repeat | 4.9614 | 5.3744 |
| Moving RELAX repeat | 6.0575 | 6.7932 |
| Moving original repeat | 5.7460 | 6.4731 |
| Static GI off | 1.8124 | 2.0391 |
| Moving GI off | 1.8840 | 2.1232 |

Mean of repeats: static **4.8873 -> 5.1153 ms** (+4.67%), moving
**5.7993 -> 6.0661 ms** (+4.60%). Subtracting matched GI-off frame means gives
**3.30 ms static / 4.18 ms moving** for the GI-on increment with NRD. This includes
integration overhead; it is not an isolated timestamp or a worst-case guarantee.

**The 0.23-0.27 ms figure is net cost, not total NRD time.** Separate GPU profiling
records about 0.904 ms RELAX + 0.079 ms prepare + 0.056 ms resolve, approximately
1.04 ms total, plus 0.117 ms raw geometry/history publication. The original
screen filter/publish interval is approximately 0.775 + 0.211 ms. These are
illustrative final profiling intervals, not averages from the unprofiled table.

## Build, GPU validation and reproduction

Compiled: Windows production Mono editor and template_release, MSVC speed
optimization, pinned NRD SDK. Native GI variants pass glslang/spirv-val and
imported-source hash checks. All 8 dispatched, entry-point-adapted diffuse NRD
SPIR-V modules pass spirv-val for Vulkan 1.2.

Real GPU: 322 input/coverage/energy/lifecycle checks pass at 1080p with TAA and
322 pass at 960x540 without TAA. These checks establish finite outputs, unchanged
cache RGB, unused hit-distance alpha, guide validity, ray budgets, sky clearing,
NRD state and odd 961x541 resize. They do not assert universal quality gain.
On/off/on toggles preserve the cache frame counter. Real-window exported 1080p
capture and Forward+ 960x540 smoke also run successfully. Accepted logs have no
engine/script or Vulkan validation errors. Headless execution is used for export
only, never as visual validation.

NRD now defaults off. For this report's NRD path use
`kiln/sponza/run.ps1 -NoSpecular -NRD -Still`; omit `-NRD` for the baseline.
The engine toggle remains `rendering/kiln/nrd`; the default
diffuse iteration count is `rendering/kiln/nrd_diffuse_iterations=4` (range 2-5).

```powershell
# Real-window diagnostic comparison, editor executable (templates ignore --script).
$engine = './bin/godot.windows.editor.x86_64.mono.console.exe'
& $engine --path kiln/sponza --rendering-driver vulkan --rendering-method kiln_deferred --gpu-validation --fixed-fps 60 --script res://tests/nrd_cache.gd -- --still --nrd --no-specular --nrd-diffuse-validate --debug-view=7 --size=1920x1080 --output=G:/Temp/kiln-nrd-check
python kiln/tools/check_nrd_cache.py G:/Temp/kiln-nrd-check
# Add --no-aa for the independent no-TAA check.

# Export after building the release template, then time without diagnostics.
& $engine --headless --path kiln/sponza --export-release 'Windows Desktop' G:/Temp/sponza-nrd.exe
python kiln/tools/benchmark_sponza.py --engine G:/Temp/sponza-nrd.exe --embedded --compare-nrd --frames 1200 --long-frames 0 --output G:/Temp/kiln-nrd-timing
```

Captures, paired images, logs, raw timings, initial source snapshot and executable
live in `G:/Temp/kiln-nrd-cache-20260918/`. Compact results and hashes are in
[nrd-cache-validation.json](nrd-cache-validation.json). Test-only reference
textures are allocated on demand and never used in normal rendering.

Unverified here: indirect specular quality, local/emissive cases, animated object
quality beyond camera motion, native Metal, D3D12, Linux, AMD/Intel and
Mobile/Compatibility. Forward+ smoke is not a motion-quality equivalence claim;
its fallback uses camera reprojection and resets on dynamic geometry changes.
Existing geometry/material limitations and cache blotches remain.
