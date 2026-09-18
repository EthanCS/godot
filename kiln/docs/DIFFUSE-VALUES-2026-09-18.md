# Indirect diffuse values and surfel blotches

The measurements below describe the initial revision. The subsequent
[cold-cache convergence fix](DIFFUSE-CONVERGENCE-2026-09-18.md) corrects startup
relighting and bootstrap history weights, and measures frames 1–512 directly.
Its ray-count-based bootstrap replaces the fixed 1/32 floor described below.

NRD is **off by default**, both in the engine setting and Sponza. It remains
available through `--nrd`, the panel, or `run.ps1 -NRD`. This change addresses the
world-space diffuse cache. Resolution, materials, sun/sky energy, surfel radius,
screen reconstruction, and the 196,608-primary-ray ceiling are unchanged.

## References and diagnosis

Reviewed the original [SIGGRAPH 2021 GIBS presentation](https://advances.realtimerendering.com/s2021/SIGGRAPH%20Advances%202021%20-%20Surfel%20GI.pdf),
including the diagrams and notes on surfel coverage, depth moments (PDF pages
41-59), adaptive sampling (74-79), and irradiance sharing (116-122).
The sharing diagram permits looking up neighboring surfel radiance before MSME;
it does not require discarding the neighbors' accumulated estimates.

Reviewed [W298/SurfelGI](https://github.com/W298/SurfelGI) at
`8361942f7d799632b32d37356b8057814456a8a2`, especially
`SurfelIntegratePass.cs.slang`, `SurfelEvaluationPass.cs.slang`, and the runtime
defaults. Its sharing reads neighboring cached radiance, uses depth rejection,
and delays contribution of new surfels. Its ray counts, coverage, and blending
defaults differ from Kiln. The repository's Sponza teaser looks smooth, but it
is a composed image, not a numeric indirect-diffuse comparison. This Falcor/DX12
implementation was not run on the Mac. No source or assets were copied into Kiln;
the external MIT license and existing SurfelPlus notices remain intact.

Three problems in Kiln contributed to the observed blotches:

- Sharing only recent, few-ray batches threw away neighbors' accumulated
  convergence. Using variance **after** sharing to control sharing also reduced
  the filter as soon as it became effective.
- Reaching 256 rays put visible surfels onto a reduced update schedule even
  when their underlying ray estimates remained noisy.
- The 256-ray bootstrap could hand an inaccurate initial mean to MSME's very
  small minimum blend, preserving a bright/dark footprint for hundreds of frames.

## Implemented

Sharing now reads an immutable snapshot of neighboring cache means in its own
dispatch, before integration writes the new means. At least 25% of its input is
fresh spatially gathered batches; during relighting this increases to 50%.
Normal/plane rejection and the existing bounded tangent-plane kernel remain.
This is a biased spatial approximation: repeated reuse can soften real lighting
within and beyond the immediate kernel. It is not an unbiased reference tracer.

The previously unused fourth component of `local_normal` stores an EMA of the
unshared batch's relative squared innovation against the short mean. This is an
uncertainty signal, not a claim of an unbiased variance estimate. It controls
sharing and scheduling independently of MSME's variance of the pooled signal.
Noisy visible surfels continue updating; stable and offscreen entries can sleep.
The original GPU budget still caps the actual primary rays.

Bootstrap remains responsive up to 2,048 actual rays at default settings, with a
1/32 minimum blend confined to that bootstrap. The threshold cannot exceed 2,048,
so it ends before the accumulated-ray counter's 4,096 saturation. Imported MSME
code is unchanged. Sharing records the fraction of old light in `shared_samples.a`
(ray count remains in `ray_results.a`); relighting compensates its integration
blend for this fraction. This avoids inadvertently doubling the temporal history.

F1 now selects **Indirect diffuse values**, mode 25. The GPU debug texture is
exactly `raw.rgb * gain`, with no exponential mapping, primary material or AO.
The display still performs its normal output color encoding and may clip bright
values. The `.bin` captures and plots below contain the actual linear values.

## Compiled

macOS arm64 Metal editor and `template_release`; all native GLSL variants,
including hardware-ray-query variants, passed glslang and spirv-val. Original
imported-source hashes passed. Both affected engine targets were rebuilt after
the final bootstrap-bound and comment changes. A self-contained Sponza PCK was
exported for the release template. Headless execution was used only for export.

## Real GPU validation

Apple M5, macOS 26.6.2, Metal 4.0 hardware ray queries. Baseline and candidate
use real windows, 1920x1080, TAA, identical fixed-60-Hz scene timelines, sun/sky,
no NRD and no indirect specular. Captures are in
`/tmp/kiln-blotches-20260918/`. Compact results and binary hashes are in
[diffuse-values-validation.json](diffuse-values-validation.json).

Measurements read `raw.bin` (world-cache gather) and `diffuse.bin` (the existing
screen reconstruction output), before primary material, AO, exposure, tone
mapping and final TAA. No conclusion about blotches uses composed screenshots.
The floor is identified geometrically; residual is luminance minus a Gaussian
low-pass, normalized by mean, with sigma 16 pixels at 1080p. Real gradients are
included, so this is not error against a path-traced reference.

The interior patch is x=34-52%, y=80-96% of the image. The older x=43-59%,
y=74-94% patch and its original threshold are reported separately; its Gaussian
neighborhood includes the plant silhouette, even after masking non-floor pixels.
The new patch does not replace the historical gate.

| Interior floor, raw irradiance/pi | Before relative residual | After | Reduction | Mean change |
| --- | ---: | ---: | ---: | ---: |
| Frame 240 | 0.022954 | 0.007892 | 65.6% | +1.13% |
| Frame 994, converged | 0.010478 | 0.006668 | 36.4% | -0.32% |
| Returned camera | 0.019188 | 0.010065 | 47.5% | -0.09% |
| Continuously changing sunset | 0.058152 | 0.043715 | 24.8% | +0.74% |

On the historical patch at frame 240, raw residual falls 43.5% and filtered
residual falls **39.1%**, passing the unchanged 35% filtered-output criterion for
this paired 1080p run. This does not retroactively pass the previous Windows
captures or the earlier 960x540 results. At convergence the historical raw
reduction is only 17.6%, and on the returned camera it is 34.3%.

Direct visual inspection of fixed-scale linear grayscale fields, floor crops
and unsmoothed numeric profiles confirms that the prominent floor bumps are
substantially reduced. Smaller undulations and edge/corner artifacts remain.

Temporal behavior is a tradeoff, not an across-the-board win. Sixteen raw
captures after frame 240 give mean per-pixel temporal standard deviation / mean
of **0.002725 -> 0.003126**, a 14.7% increase (0.273% -> 0.313% of mean).
The existing filtered signal changes 0.002452 -> 0.002830 (+15.4%). These include
TAA's raster jitter and continuing cache convergence, but exclude final TAA and
8-bit display quantization. The more responsive bootstrap contributes to this.

Validation also covers 300 focused structural/signal checks, finite/nonnegative
raw and filtered values, actual ray totals and budget, camera turns, 961x541
resize and restoration. A moving frame passes 2,048 independent brute-force
grid queries. The separate light-response suite passes 18 checks: the floor
falls from 0.050075 to 0.00001051 after 32 dark frames, stays below 0.1% of day
at frame 128, and returns within 0.3% of day after restoring light. The mode-25
debug texture matches the linear raw values times gain. Accepted validation logs
have no engine, script or GPU-validation errors.

## Cost and remaining scope

Quality is improved partly by spending more of the existing ray budget. At the
static 1080p capture, actual primary rays increase from **77,104 to 196,608**;
at sunset both use 196,608. The ceiling is unchanged, but the workload is not.
No performance improvement, target frame rate, or unchanged frame time is claimed.
Shadow and continuation rays remain additional work.

Depth-moment visibility from GIBS is still unimplemented. Normal/plane rejection
does not solve every corner or disconnected coplanar-surface case. Strong spatial
reuse can soften true light transitions. This work does not establish zero
blotches in every view or equivalence to the reference project's output.
Windows/Vulkan real-GPU behavior, Linux, other GPU vendors, active NRD,
local/emissive workloads and full reflection quality were not validated here.
The retained Forward+ path receives a release-runtime smoke check, not a new
quality/performance comparison against deferred rendering.

## Reproduce

```sh
./kiln/sponza/run.command --still --no-specular --debug-view=25

bin/godot.macos.editor.arm64 --path kiln/sponza --rendering-driver metal \
  --rendering-method kiln_deferred --fixed-fps 60 \
  --script res://tests/diffuse_focus.gd -- --still --no-specular \
  --capture-diffuse-values --debug-view=7 --size=1920x1080 --output=/tmp/diffuse-after
python3 kiln/tools/check_diffuse_focus.py /tmp/diffuse-after
python3 kiln/tools/compare_diffuse_values.py /tmp/diffuse-before /tmp/diffuse-after \
  --output /tmp/diffuse-comparison --plot

bin/godot.macos.editor.arm64 --path kiln/sponza --rendering-driver metal \
  --rendering-method kiln_deferred --gpu-validation --fixed-fps 60 \
  --script res://tests/diffuse_response.gd -- --still --size=960x540 \
  --output=/tmp/diffuse-response
python3 kiln/tools/check_diffuse_response.py /tmp/diffuse-response
```

The comparison requires matching frame counts, sizes and options. Mode 7 keeps
the normal TAA raster configuration for these readbacks; its composed display is
not used by the comparison. `--plot` additionally requires matplotlib.
