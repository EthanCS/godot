# Full-rate reflections with NRD, 2026-09-18

This revision enables and validates indirect specular alongside the existing
surfel-cache diffuse. The workload remains sun/sky only, with no local-light or
emissive cases. Floor roughness 0.06, 0.18, 0.45 and 0.85 are tested separately.

Deferred now uses one material-demodulated RELAX_SPECULAR signal instead of two
complete denoisers for its correlated Schlick basis signals. Normal NRD rendering
traces **two specular samples per valid pixel at full resolution**. The former
checkerboard mode is explicitly opt-in: measured brightness and motion recovery
were not equivalent to full-rate input. Diffuse cache generation, sharing, MSME,
ray guiding and its 196,608-primary-ray budget are unchanged.

Sponza's panel exposes floor roughness, indirect specular and NRD switches. The
reflection debug view now retains TAA, as the lit view does; previously selecting
view 28 incorrectly treated it as a categorical debug view, disabling TAA and
recreating viewport resources. Turning GI off also explicitly clears reflection
outputs, including previously populated NRD textures.

## Material and distance contract

The tracer still samples the GGX visible-normal distribution and produces the
same raw Schlick integrals A and B. Using the actual G-buffer F0, F90 and the
engine's existing DFG lookup, preparation computes:

```
S = F0 * A + F90 * B
a = DFG.y - DFG.x
b = DFG.x
factor = F0 * a + F90 * b
R = S / max(factor, 1e-8)
```

RELAX_SPECULAR filters R. Resolve emits `A' = a * R'` and `B' = b * R'` for the
existing material composition. Thus `F0*A' + F90*B' = factor*R'`; an identity
denoiser reproduces the traced signal, apart from floating-point storage error.
The engine's multiscatter compensation remains in final composition. Denoised
A'/B' are an equivalent DFG basis for the current receiver, rather than two
independently filtered Monte Carlo integrals. This does not replace reflection
transport with a DFG/environment lookup: all incident radiance still comes from
the traced scene, sky and surfel cache.

The material factors come from the same primary material and roughness used by
composition. The adaptation follows the
[NRD material demodulation principle](https://github.com/NVIDIA-RTX/NRD/blob/792eff196afdd350fd9c3f862119017ccb438a0e/README.md#quick-start),
using Godot's DFG rather than an unrelated BRDF approximation. No NRD SDK source
was copied into engine source. NRD remains the externally built, licensed
4.17.3 dependency at `792eff196afdd350fd9c3f862119017ccb438a0e`.

Specular alpha is actual first-hit distance, with a 1,000-unit sky miss. For
multiple samples the minimum positive hit distance is retained, consistent with
the pinned SDK's `NRD_FrontEnd_SpecHitDistAveraging_*` helpers. It is not divided
by PDF or normalized as REBLUR hit distance. Specular uses the SDK prepass,
anti-firefly pass, 30-frame history (8 during relighting), and 6 A-trous iterations
in full-rate mode. Normal, roughness, view depth, unjittered motion and separate
camera jitter follow the previously validated adapter contract.

Forward+ retains the independent Schlick denoisers because its prepass does not
provide the primary material G-buffer needed for this demodulation. The original
post-filter remains available with NRD off. Diffuse remains RELAX_DIFFUSE with
its own full-rate inputs and unchanged settings.

## Quality measurements

The real-window suite warms each roughness for 160 frames, captures the lit and
isolated reflection outputs, records eight temporal samples, moves the camera
through a deterministic lateral excursion, and compares 8 versus 96 settled
frames. It also tests zero F0, metallic floor, reflection on/off/on, odd 961x541
resize and GI off. TAA is enabled, but numerical reflection statistics are
computed from linear buffers before final TAA and multiscatter compensation.

Masks select horizontal floor pixels at the authored floor roughness; unrelated
horizontal ledges are excluded. No quality threshold is loosened to hide those
surfaces or to accept noise. Temporal and settling statistics are descriptive,
not errors against a path-traced reference. Runs share the deterministic scene
timeline, but GPU cache allocation is not bitwise deterministic across processes.

At 960x540, mean temporal pixel standard deviation divided by mean floor
reflection luminance (smaller is more stable):

| Roughness | Original filter | Two NRD specular signals | One demodulated NRD signal |
| --- | ---: | ---: | ---: |
| 0.06 | 7.195% | 0.694% | 0.693% |
| 0.18 | 18.557% | 0.712% | 0.742% |
| 0.45 | 63.859% | 0.629% | 0.625% |
| 0.85 | 118.476% | 0.656% | 0.581% |

At 1920x1080 the new default's corresponding values are 0.541%, 0.627%,
0.453%, and 0.415%. Its normalized floor difference between 8 and 96 settled
frames is 5.516%, 6.648%, 4.339%, and 2.661%. These are not zero-ghosting results.
The identity-denoiser material round-trip's worst per-capture component p99
relative error is 0.112% (denominator floor 0.001), within half-float precision.

The single-signal optimization is **not uniformly equivalent in quality** to
the two-signal adapter. Floor mean reflection differs by at most about 1.81%,
but non-floor surfaces show more residual variation. At roughness 0.18 and
960x540, non-floor temporal variation is 75.690% with the original filter,
5.309% with two NRD specular signals, and 18.154% with one demodulated signal.
The corresponding settled difference is 88.147%, 9.610%, and 26.984%.
Non-floor mean specular luminance is 0.000201, 0.000340, and 0.000391 respectively;
without reference convergence these brightness differences are not proof of
better energy accuracy. At 1080p the single-signal non-floor temporal variation
is 11.775%. The measurements include G-buffer jitter and material remodulation,
so they do not isolate Monte Carlo noise alone.

Visual review confirms that the difference is visible on walls, upper masonry
and foliage in the reflection-only output at a common 16x visualization gain.
It is much less prominent in the normal lit output, but remains a quality
limitation. The 1.5-2.0 ms optimization below therefore has a recorded tradeoff;
it is not claimed as a free, equal-quality improvement. The old adapter stays
available through `--nrd-separate-specular` for comparison.

The 960x540 comparison with the former checkerboard mode showed 12-17% lower
mean floor reflection than full-rate NRD. At roughness 0.06 its normalized
8-versus-96-frame settled difference was about 0.245 versus 0.060 with full rate.
Accordingly checkerboarding is not included in the normal-performance claims.
It remains available as an explicitly different quality/performance option.

The sharper floor produces identifiable column/curtain reflections, 0.18 broadens
them, and 0.45/0.85 produce progressively wider, weaker highlights. Raw input and
the retained filter contain substantial specular noise on rough surfaces; NRD
greatly reduces it. Broad diffuse-cache blotches remain a separate limitation.
Neither the comparison nor material round-trip checks establish perfect energy
convergence, zero ghosting, or correctness for every material/lighting setup.

## Performance

Production Windows x86_64 Mono export, RTX 5070 Ti, Vulkan, 1920x1080, TAA.
All on/off comparisons below retain full-rate two-sample reflections, identical
diffuse budgets, shadows, material settings and deterministic camera/sun timeline.
Each run has 180 warmup + 1,020 measured frames, with no validation layers,
profiling, capture readbacks, concurrent rendering or builds. Reported values
are whole-frame intervals; render frame counters confirm that windows stayed
active. Separate profiling is not mixed into these timings.

Mean whole-frame milliseconds, single sweep (26 total runs including controls):

| Floor roughness | Static, original filter | Static, NRD | Moving, original filter | Moving, NRD |
| --- | ---: | ---: | ---: | ---: |
| 0.06 | 7.482 | 8.990 | 8.713 | 10.448 |
| 0.18 | 7.461 | 9.131 | 8.897 | 10.383 |
| 0.45 | 7.713 | 9.166 | 9.028 | 10.665 |
| 0.85 | 7.593 | 9.122 | 8.786 | 10.560 |

Roughness 0.18 with repeated baseline/default measurements averaged:

| Configuration | Static whole frame | Moving whole frame |
| --- | ---: | ---: |
| GI off | 1.702 ms | 1.768 ms |
| Cache diffuse + NRD, no indirect specular | 4.756 ms | 5.617 ms |
| Diffuse + specular, original filters | 7.465 ms | 8.885 ms |
| Diffuse + single-signal specular NRD | 9.120 ms | 10.362 ms |
| Diffuse + older two-signal specular NRD | 10.623 ms | 12.371 ms |

Net NRD overhead relative to the original filters is **1.656 / 1.477 ms**
(static / moving). Avoiding the duplicate specular denoiser saves **1.502 /
2.009 ms** relative to the old NRD adapter with the same full-rate ray input.
Adding reflections and their NRD to the diffuse-only NRD path costs **4.364 /
4.746 ms**. All of these are differences of whole-frame intervals, not sums
of isolated GPU timer scopes.

The full lighting path adds **7.418 / 8.594 ms** relative to GI off. The requested
few-millisecond complete-GI target is **not met**. A separate instrumented run
places specular tracing at about 2.48 ms and diffuse + specular RELAX at 2.38 ms,
plus 0.146 ms preparation and 0.047 ms resolve. This is illustrative GPU profiling,
not the uninstrumented performance result above. NRD's total GPU duration must
not be confused with its net overhead after replacing the original filters.

## Validation status and reproduction

Implemented: full-rate reflection/NRD integration, one deferred specular signal,
material demodulation/recomposition, explicit checkerboard option, UI controls,
reflection-view TAA and GI-off clearing.

Compiled: Windows production Mono editor and template_release with the pinned
SDK. Native shader variants and imported-source hashes pass validation. All 17
captured diffuse/specular NRD SPIR-V modules pass spirv-val for Vulkan 1.2.

Real GPU: final 1080p editor suite passes 534 checks, including material
round-trip, cache input preservation, actual hit distance, finite buffers,
ray budget, zero-F0 extinction, specular toggle, odd resize and GI-off clearing.
At 960x540 the full-rate single-signal suite passes 534, the original-filter
suite 326, the old two-signal suite 482, and the optional checkerboard suite 534.
The numerical gates check contracts and lifecycle; they do not certify all
image-quality aspects. Non-floor stability measurements above are reported
even though they are not failure gates.

Visually reviewed: all four roughnesses in real 1080p exported lit captures,
reflection-only comparisons, camera-motion capture, UI controls and Forward+
smoke output. Actual UI signals change roughness, indirect specular and NRD;
the UI smoke prints `[SPECULAR_UI] passed`. Forward+ reports the old two-signal
adapter active. Accepted final runs have no engine/script or Vulkan validation
errors. Earlier failed compilation and an accidentally launched previous binary
are retained as investigation logs, excluded from acceptance/performance claims.

Unverified: Metal, D3D12, Linux, AMD/Intel, Mobile/Compatibility; local/emissive
workloads and complex moving-object quality. The existing geometry/material
limitations remain. Forward+ smoke does not establish equal motion quality or
performance; it uses camera reprojection and the older two-signal adapter.

Interactive:

```powershell
kiln/sponza/run.ps1 -Still -Roughness 0.18
# Use the panel to adjust roughness, indirect specular and NRD independently.
```

Diagnostics and matched performance:

```powershell
$engine = './bin/godot.windows.editor.x86_64.mono.console.exe'
& $engine --path kiln/sponza --rendering-driver vulkan --rendering-method kiln_deferred --gpu-validation --fixed-fps 60 --script res://tests/specular_focus.gd -- --still --nrd --size=1920x1080 --output=G:/Temp/kiln-reflection-check
python kiln/tools/check_specular_focus.py G:/Temp/kiln-reflection-check
& $engine --headless --path kiln/sponza --export-release 'Windows Desktop' G:/Temp/sponza-reflections.exe
python kiln/tools/benchmark_sponza.py --engine G:/Temp/sponza-reflections.exe --embedded --compare-reflections --frames 1200 --long-frames 0 --output G:/Temp/kiln-reflection-timing
```

Add `--no-nrd --full-specular-rate` for equal-ray baseline diagnostics.
`--nrd-separate-specular` selects the older two-denoiser adapter without changing
tracing. `--nrd-checkerboard` enables the explicitly lower-sampling mode;
`--nrd-reference` forces full rate. The engine settings are
`rendering/kiln/nrd_combined_specular` (default true) and
`rendering/kiln/nrd_specular_checkerboard` (default false).

Captures, logs, initial source snapshot, exported executable and performance
JSON live in `G:/Temp/kiln-nrd-specular-20260918/`. Compact validation results and
source/binary hashes are recorded in
[nrd-reflections-validation.json](nrd-reflections-validation.json).
