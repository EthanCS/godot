# Phase 1 development status

Implementation and validation checkpoint, 2026-09-16. Phase 1 is not yet accepted.

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
| Dynamic numerical GI | `/tmp/kiln-dynamic-check-v1` | All 26 checks passed; thresholds from source test; compact values in dynamic-check-baseline.json |
| Original 4.6.2 source reference | `/tmp/kiln-source-fiveviews`, `$TMPDIR/oc_shots/island_custom_gi/reference_*` | Five original camera views, no AA, lit/direct/indirect/albedo captured via isolated C# study adapter |
| Source baseline first-dark-frame check | `/tmp/kiln-source-reference-run3.log` | Original quick check failed its first-dark-frame assertion; not reported as passed |
| Native source-view comparison | `/tmp/kiln-reference-native` | Same courtyard camera, exposure and original sky model visually inspected; AA differences in initial source quick capture |
| 1001×703 with TAA | `/tmp/kiln-oddsize` | Normal exit, no renderer errors, actual 96 omni + 32 spot uploaded, no overflow |
| Continuous resize/camera changes | `/tmp/kiln-lifecycle` | Repeated 1080p/1001×703 transitions and camera cuts, no errors |
| Material/transparent/refraction chart | `/tmp/kiln-material-contract-*` | Both renderer outputs inspected; no rendering errors |
| Five-minute all-dynamic run | `/tmp/kiln-five-minute-v1` | 305 seconds, 4186 frames, 1080p/TAA, all 128 lights + 8 local shadows; no errors; static BVH version stayed 1 |
| Local standalone export | `/tmp/kiln-export/KilnIsland.app`, `/tmp/kiln-export-run` | Release template + exported PCK launched outside repository; full island rendered; normal exit |

Dynamic checks cover emitter on/off/color/translation, old-position clearing,
solid dynamic occlusion, omni/spot indirect light, finite floating-point SH,
continuous motion, stationary-versus-cold bit equality and static rebuild equality.
They are small controlled scenes, not a claim that every island temporal case is
accepted. The five-minute run used the earlier orbit/approach camera; the newer
configured courtyard tour still needs a full final run.

## Performance evidence

Three-repeat A/B pilot is running; full matrix remains pending. GPU timestamps
returned zeros on this Metal backend and are reported as unavailable (`null`/N/A),
not zero cost. CPU profile values describe submission, not GPU stage duration.
Timing runs perform no image/buffer readbacks. The five-minute run included
captures and concurrent compilation, so its timings are not a benchmark.
No 1080p/60 FPS target has passed. Dynamic software-BVH transport is substantially
more expensive than stationary convergence; this must remain visible in reports.

## Remaining acceptance and limits

- Full 32/128/512/1024-light scattered/dense A/B matrix, zero/fixed shadows and
  identical GI budgets; final dynamic performance and full source-view diffs.
- Quantitative TAA fast flash/disocclusion, offscreen emitter check, direct-shadow
  contact thresholds, reload/memory plateau, and final camera path coverage.
- Native GI texture-dependent reflectance/alpha holes, arbitrary material vertex
  deformation and skinned/multimesh capture are not supported. The complete
  imported island uses uniform, opaque materials; raster cutout/normal maps are
  separately tested. Broader GI material admission still needs tightening.
- Deferred MSAA, XR/multiview, reflection probes and arbitrary custom light shaders
  are unsupported. SDFGI/VoxelGI/lightmap/SSR mixing is outside this contract.
- Public redistribution terms for imported assets and recovered shader sources
  remain unestablished. Local development/export is authorized; no payload has
  been published. Source game remains read-only.

Temporary artifacts may expire. Preserve implementation, compiled, visually
verified, numerical checks and unsupported features as separate claims.
