# Imported GI proxies, hardware queries and Sponza TOD

Implementation and validation record, 2026-09-17. Sponza is the active GI
acceptance scene. The default demonstration uses only sun and sky; local lights
and emissive test objects are an optional, separately checked workload.

## Implemented

### Automatic import and material updates

- OBJ and scene importers store a serialized `KilnGIProxy` in each imported
  mesh's `kiln_gi_proxy` metadata. It contains an inspectable `proxy_mesh`,
  solid-color materials, source material references and geometry fingerprints.
  Generation happens before external mesh saves, after import postprocessing,
  and after single-mesh merging. Existing imports are invalidated by the importer
  settings version. The proxy survives save/load and game export.
- UV/normal seams are welded before meshoptimizer simplification. Material and
  open geometric boundaries are preserved. The default triangle ratio is 0.12
  and relative error is 0.001; error/boundary constraints take precedence over
  the ratio. Cached geometry is independent of visible LODs.
- Source material color, metallic, emission, texture replacement and texture
  pixel edits refresh solid proxy materials automatically, even without a GI
  node. Relevant BaseMaterial3D setters emit resource changes. Source surface
  material replacement reconnects the proxy to the new material. Texture colors
  are averaged in linear space with alpha weighting. Material-only edits reuse
  simplified geometry and acceleration structures.
- `KilnGIWorld` reuses matching imported proxies. Runtime-created or changed
  geometry retains automatic runtime generation. Per-instance material overrides
  affect that instance's transport without recoloring a shared proxy. Transform,
  visibility and resource changes update the appropriate static/dynamic subtree;
  removed resources release their caches and signal connections.

### Hardware backend and compute fallback

- On supported Vulkan devices, `VK_KHR_ray_query` traces the same proxy triangles
  using actual GPU acceleration structures. Static and dynamic packed geometry
  have separate BLAS, referenced by a TLAS. Committed instance/primitive IDs map
  to the shared diffuse/emissive transport attributes. Hardware primary and
  visibility queries run inside the existing compute integration shaders.
- `KilnGIWorld.set_query_backend(0)` selects automatically; `1` forces software;
  `2` prefers hardware with fallback. Unsupported capability or failed hardware
  setup uses the software path. Statistics and captures report the actual backend.
- Software topology uses CPU binned SAH; bottom-up bounds are built by compute
  shaders and GPU nearest-hit/visibility queries traverse the threaded tree.
  SAH partitioning itself is not claimed to run on GPU. This tree remains
  available for fallback and explicit hardware-versus-software diagnostics.
- Material updates do not rebuild BLAS/TLAS. Geometry changes rebuild the
  affected packed subtree. This is not yet a reusable per-object BLAS system
  with transform-only TLAS refits.

### Diffuse transport and temporal stability

Sun/sky, analytic local lights and material emission feed the same diffuse GI
integrator on both backends. Local lights use incident-power importance sampling
with visibility and inverse-PDF weighting. Emissive surfaces use area/power
next-event sampling. Direct lighting and indirect transport remain separate;
no blanket ambient term or baked lightmap substitutes for indirect transport.

Temporal luminance moments guide three geometry-aware a-trous passes. Receiver
validation, disocclusion sampling, bounded changing-light histories and temporal
full-resolution publication handle TOD and camera motion. GI resolution divisor
4 is the balanced demo default; 2 is the higher-resolution mode. Direct lighting
and AO retain their own resolution. Resize/quality/backend changes reset the
required history. The velocity history copy now uses RG16F-compatible storage
and the internal viewport size, fixing a Vulkan validation error exposed by TAA.

Forward+ ordinary opaque StandardMaterial3D receives the same native GI when a
KilnGIWorld is active, permitting equivalent-quality comparisons with deferred.

## Reference and design choices

Tomasz Stachowiak's [GPC 2024 Tiny Glade presentation](https://graphicsprogrammingconference.com/archive/2024/)
([recording](https://www.youtube.com/watch?v=jusWW2pPnA0)) is the public reference
for dynamic diffuse GI and continuous TOD. Our boundary-preserving simplified
triangle proxies and averaged colors are an independent implementation of the
requested approach, not a reproduction of Tiny Glade's private representation.
No new recovered game code or assets were imported. Earlier shader provenance
remains in `gi-provenance.json`.

The hardware path follows the Khronos [GLSL ray query extension](https://github.com/KhronosGroup/GLSL/blob/main/extensions/ext/GLSL_EXT_ray_query.txt)
and [Vulkan ray query feature contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceRayQueryFeaturesKHR.html).

## Compiled

Windows x64 production editor and release export template, MSVC 14.3, SDK
10.0.26100.0, Mono enabled. GodotSharp Debug/Release APIs and SDK were regenerated
and built successfully. Current native binaries include the final importer and
hardware query changes. No new macOS/Metal, D3D12, Mobile or Compatibility build
or execution pass is claimed.

## Real GPU verification

Host: NVIDIA GeForce RTX 5070 Ti, Windows x64, Vulkan 1.4.341,
driver 32.0.16.1088. Numerical checks and visual inspection are separate evidence.

| Check | Result and scope |
| --- | --- |
| Import lifecycle, without a GI node | OBJ and glTF proxies persist; source color, texture pixels, emission, material replacement and reload pass. Single-mesh glTF merging preserves both source meshes in its proxy. FBX uses the generic scene hook but has no separately tested fixture. |
| Sponza pure sun/sky TOD | Hardware 30/30 including software image comparison; software 20/20. Fixed views at 06:30, 09:00, 12:00, 17:30 and 21:00; GI on/off and indirect-only; rotating camera/day cycle. Zero local lights or emissive props. |
| Imported geometry use | 25 imported surfaces, zero runtime proxy builds. 262,267 source triangles become 107,675 cached proxy triangles, 107,667 nondegenerate admitted triangles. |
| Hardware/software GPU queries | 2,048 deterministic nearest-hit and short visibility queries per hardware capture; zero mismatches. Sponza maximum hit-distance error 0.00000787 world units. |
| Hardware/software images | Maximum mean display-RGB difference across the five TOD views: 0.0000001575, excluding HUD. |
| Noise and motion settling | Pure-TOD stationary display-RGB standard deviation 0.0017253, below the predefined 0.005 bound. Settled-32 versus reference MAE 0.00001373, below 0.035. This is a bounded measurement, not a universal zero-noise claim. |
| Multiple lights and emission | Hardware 60/60; software 60/60, including isolated indirect sources, movement, decay and material-only geometry-version invariants. |
| Controlled dynamics / temporal tests | 29/29 and 9/9. Room fixtures, without the island model; include offscreen emission, moving occluders, omni/spot bounce and turn-off decay. |
| Proxy/backend lifecycle | Texture edits, source replacement, static transforms, mesh edits, visibility/empty world, resize and repeated software/hardware switches pass. Material edits leave hardware build counts unchanged. |
| Forward+ equivalent TOD | 30/30 including deferred image comparison at equal quality. |
| Vulkan validation | Final hardware TOD, full suite and lifecycle logs contain no errors, warnings or VUID reports. |
| Exported executable | Standalone embedded-PCK release launched outside the repo, real hardware backend, 1001x703 output with 251x176 GI receivers; imported proxies reused, query diagnostics pass. Functional check, not a performance result. |

Visual inspection covers Sponza daytime GI, indirect-only response, warm evening
lighting in the continuous TOD video, night and exported runtime. The review
gallery provides the five times, a GI on/off wipe and indirect-only images. The
20-second video contains a complete 24-hour cycle at a fixed camera. Camera
rotation and local/emissive behavior have separate GPU captures and checks.
Readback captures are not used for timing.

## Bounded performance measurements

Sponza **sun/sky TOD plus camera orbit**, 1920x1080, TAA, zero local lights and
emissive props, VSync off. Serial runs, 60 warmup frames then 360 measured frame
intervals, deterministic 1/60 simulation steps, no readbacks or concurrent builds.
These are short whole-frame wall-time measurements, not per-stage GPU timings
or a sustained performance pass.

| Backend | GI receiver divisor | Mean ms | P95 ms |
| --- | ---: | ---: | ---: |
| Hardware ray query | 2 | 3.631 | 4.037 |
| Software BVH | 2 | 18.188 | 21.781 |
| Hardware ray query | 4 | 2.550 | 2.830 |
| Software BVH | 4 | 6.768 | 7.762 |

Compare backends within the same divisor for equal quality. Divisor 4 changes
indirect sampling resolution; its improvement is not solely a traversal gain.
No Mac performance conclusion follows from these Windows measurements.

Exact binary/source hashes, reports and temporary evidence paths are recorded in
[gi-tod-validation.json](gi-tod-validation.json). The previous software-only,
multi-light measurements in [gi-validation.json](gi-validation.json) are a
historical 2026-09-16 snapshot, not results for the current binaries or workload.
Captures, video, compiled binaries and downloaded assets remain in temporary or
ignored storage. Reproduction commands are in [Sponza README](../sponza/README.md).

## Unsupported and remaining work

- Metal hardware ray tracing is not implemented by this Vulkan change. The
  software fallback remains for devices/backends without ray queries. Actual
  target-Mac compilation, visual checks and performance measurements are still
  required; no Mac or universal 1080p/60 target pass is claimed.
- Rigid opaque proxies approximate texture detail with constant colors. UV-local
  ray texture lookup, exact alpha-cutout silhouettes, blended transmission,
  skinning, blend shapes, MultiMesh and arbitrary shader deformation are outside
  the current contract. ShaderMaterial transport requires the existing explicit
  uniform contract; imported material preview uses BaseMaterial3D.
- This is diffuse/low-frequency SH GI with the existing approximate specular lobe,
  not arbitrary multi-bounce path tracing, sharp ray-traced reflections or
  caustics. Fine proxy occlusion, light leaks and fast-motion noise still require
  evaluation on production geometry beyond these acceptance scenes.
- Low-level GPU-only geometry or material edits without Resource notification
  need explicit invalidation or a dedicated proxy. Import simplification ratios
  are targets rather than triangle-count guarantees.
- The original Sponza OBJ import is not an opacity/bump/PBR conformance reference.
  Provenance and redistribution constraints remain in its README; no asset
  payload has been published.
