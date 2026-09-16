# Phase 1 development status

Implementation and validation record, 2026-09-16. Functional checks and performance results are reported separately. See [PERFORMANCE.md](PERFORMANCE.md) for the complete A/B measurements.

## Current delivery scope

Per the user's 2026-09-16 scope update, this checkpoint accepts the non-GI renderer
and standalone demo first. GI defaults off, remains opt-in with G / `--gi`, and is
not part of the current acceptance claim. Its existing implementation, earlier
checks and unmet performance target remain recorded below; further GI work is deferred.

## Implemented

- Selectable `kiln_deferred`: genuine six-attachment opaque/cutout G-buffer,
  clustered fullscreen direct lighting, upstream directional/local shadows,
  forward transparent/additive/refraction stage and HDR post/TAA.
- Native `KilnGIWorld`: independently versioned static/dynamic software BVH and
  light grid; dynamic authored emission, omni/spot bounce, original SH transport,
  temporal/world caches, source material response, and independently toggled AO.
- Separate Forward+ path with the same native Kiln GI and authored BRDF for A/B.
- Independent 797-surface/53-material island, default 128 lights (96 omni/32 spot),
  8 local shadow lights, 12 emissive bodies and four occluders. Deterministic camera,
  TOD, lights/emission; four isolation presets; count/shadow/layout controls.
- Raw diagnostic capture, actual light upload/overflow counts and CPU profiling.
  Unsupported opaque shader features are rejected explicitly. See PIPELINE.md.

## Compiled

macOS arm64 editor **and release export template**, Metal enabled, Vulkan disabled,
.NET disabled. Commands: `scons platform=macos target=editor arch=arm64 vulkan=no
metal=yes -j8`; replace target with `template_release` for the runtime. Untouched
pinned upstream was built separately before implementation. Windows/Vulkan have
not been built or run.

## Real GPU verification completed

Host: Apple M5, Metal 4.0. Runtime RD capability query returned false for both ray
query and ray tracing pipeline; actual backend is software BVH.

| Check | Evidence in temporary storage | Result / scope |
| --- | --- | --- |
| Opaque island G-buffer/direct lighting | `/tmp/kiln-deferred-material-fix` | Visually inspected; fixed initial binding/decode failures |
| Native GI and local-only indirect | `/tmp/kiln-native-gi2`, `/tmp/kiln-isolated-local-v3` | Nonzero indirect with sun/sky/all emission disabled in local-only case |
| XeGTAO independent diagnostic | `/tmp/kiln-ao-v2` | Contact/detail AO visually inspected; push-constant issue fixed |
| Dynamic numerical GI | `/tmp/kiln-validation-accepted/dynamic` | All 29 checks passed; thresholds from source test; compact values in dynamic-check-baseline.json |
| Original 4.6.2 source reference | `/tmp/kiln-source-fiveviews`, `$TMPDIR/oc_shots/island_custom_gi/reference_*` | Five original camera views, no AA, lit/direct/indirect/albedo captured via isolated C# study adapter |
| Source baseline first-dark-frame check | `/tmp/kiln-source-reference-run3.log` | Original quick check failed its first-dark-frame assertion; not reported as passed |
| Native source-view comparison | `/tmp/kiln-reference-final`, `/tmp/kiln-reference-diff-final` | Five matched 1080p/no-AA views; lit display-RGB MAE .00059–.00368, indirect .000165–.001013 after emission masking; descriptive values, no fitted acceptance threshold |
| 1001×703 with TAA | `/tmp/kiln-oddsize` | Normal exit, no renderer errors, actual 96 omni + 32 spot uploaded, no overflow |
| Continuous resize/camera changes | `/tmp/kiln-lifecycle` | Repeated 1080p/1001×703 transitions and camera cuts, no errors |
| Material/transparent/refraction chart | `/tmp/kiln-material-contract-*` | Both renderer outputs inspected; no rendering errors |
| Five-minute all-dynamic run | `/tmp/kiln-five-minute-v1` | 305 seconds, 4186 frames, 1080p/TAA, all 128 lights + 8 local shadows; no errors; static BVH version stayed 1 |
| TAA flash/disocclusion and offscreen source | `/tmp/kiln-validation-accepted/temporal` | All 9 checks passed; off32 residual/lit .000336, maximum .00784; settled32 MAE .000540; confidence median 256, explicit-reset maximum 1 |
| Direct-shadow contact | `/tmp/kiln-shadow-contact` | Two 81-pixel seams pass <2% shadow/lit ratio; measured zero |
| Scene reload and BVH query | `/tmp/kiln-validation-accepted/reload.log` | Three lifetimes; VRAM plateau 762,511,360 bytes; 128 deterministic tree/brute-force rays match exactly |
| Final camera tour and exported app | `/tmp/kiln-final-tour` | 305.10 seconds, 2,801 frames, 146 captures, all dynamics/TAA/AO; no errors; geometry version 1 throughout |
| Camera path clearance | `/tmp/kiln-validation-accepted/camera.json` | 49 center/offset rays against 137 collision meshes; no island intersection (CPU geometry check) |
| GI admission and quality switching | `/tmp/kiln-validation-accepted/admission.json` | Unsupported material excluded; 1→8→1 ray budget, metal-only changes, mesh replacement and in-place edit + rebuild passed |
| Additional local shadow budgets | `/tmp/kiln-shadows-16`, `/tmp/kiln-shadows-32` | Actual uploaded shadow counts 16 and 32, zero overflow, normal exit |
| Local standalone export | `/tmp/kiln-final-export/KilnIsland.app`, `/tmp/kiln-panel-settled` | Release template + exported PCK launched outside repository; full island rendered; normal exit |

Dynamic checks cover emitter on/off/color/translation, old-position clearing,
solid dynamic occlusion, omni/spot indirect light, finite floating-point SH,
continuous motion, stationary-versus-cold bit equality and static rebuild equality.
These are bounded controlled scenes. Continuous island coverage is separately
recorded by the final courtyard tour, including day/night, close occlusion edges,
moving receivers/emitters and pullback. Captures establish functional visual
behavior, not a 60 FPS performance pass.

## Performance evidence

The complete 192-run static matrix uses frozen editor/PCK files from source
commit `f4e93aac42`. The benchmarked rendering implementation is `06843311c6`, which also
fixes TAA stationary history accumulation and lighting decay; the final dynamic
comparison and six-run no-AA spot check are recorded separately in PERFORMANCE.md.
GPU timestamps returned zeros on this Metal backend and are reported as unavailable
(`null`/N/A), not zero cost. CPU profile values describe submission, not GPU stage
duration. Timing runs perform no image/buffer readbacks. Five-minute visual runs
include captures and are not benchmarks. No 1080p/60 FPS performance pass is claimed.

## Remaining acceptance and limits

- The default all-dynamic 1080p/60 FPS optimization target is not met on the M5;
  paired GI-off/on replays identify dynamic GI as the largest incremental cost.
  See PERFORMANCE.md for measured frame times, available profiling and next work.
- Native GI texture-dependent reflectance/alpha holes, arbitrary material vertex
  deformation and skinned/multimesh capture are not supported. The complete
  imported island uses uniform, opaque materials; raster cutout/normal maps are
  separately tested. GI requires an explicit uniform material contract.
- Deferred MSAA, XR/multiview, reflection probes and arbitrary custom light shaders
  are unsupported. SDFGI/VoxelGI/lightmap/SSR mixing is outside this contract.
- Public redistribution terms for imported assets and recovered shader sources
  remain unestablished. Local development/export is authorized; no payload has
  been published. Source game remains read-only.

The rendering regression suite at `06843311c6` passed at `/tmp/kiln-validation-accepted/passed.json`.
The stationary TAA fix was checked again against the predeclared thresholds;
no thresholds were relaxed. The earlier 305-second tour predates that fix and
remains separate evidence of continuous motion; it is not relabeled as a run of
the final binary. Delivery `b3129706fd` additionally makes GI opt-in and fixes CLI help callbacks
after editor teardown; renderer shaders/transport are unchanged from the timed
build. Import, export and a real-GPU non-GI launch are checked again for delivery.
Exact binary and source identities are in validation-host.json.

Temporary artifacts may expire. Preserve implementation, compiled, visually
verified, numerical checks and unsupported features as separate claims.
