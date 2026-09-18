# Kiln rendering pipeline

Engine development branch based on Godot **4.7.2-stable**, pinned in
[`upstream.json`](upstream.json). The upstream README and license remain applicable.

## Phase 1

Implement an engine-level hybrid deferred rendering path and integrate Kiln GI.
Opaque and cutout geometry write a standard G-buffer for clustered deferred lighting;
transparency, additive effects, and refraction use an explicit forward stage.
Retain a working Forward+ path for controlled comparisons.

The standalone Sponza GI benchmark defaults to sun/sky diffuse illumination,
continuous time of day and an automatic camera, with a manual TOD slider.
The optional multi-light workload adds 12 local lights (four shadowed), three
emissive meshes and moving occluders.
The historical island stress presets remain in kiln/demo.
Kiln must update correctly while camera, geometry, and lighting move together.

Milestones:

1. Build the pinned engine and establish a reproducible independent demo baseline.
2. Implement material G-buffer outputs, motion data, clustered direct lighting,
   shadows, and HDR composition with correct transparency/post-processing order.
3. Integrate Kiln GI using engine material and motion data; validate emissive and
   local analytic light bounce, dynamic visibility, and temporal stability.
4. Implement deterministic demo timelines, isolated lighting presets, and diagnostics.
5. Validate real GPU output, continuous operation, reload/resize/export behavior,
   and fair Forward+ versus deferred performance with raw measurements.

The 1080p/60 FPS target applies to the default load on explicitly documented
hardware; it is an optimization target, not a measured result. Vulkan hardware
ray queries and native Metal hardware ray queries are implemented with a compute
software-BVH fallback. See [Metal support and validation](docs/METAL-RAY-QUERY.md).
See also [Metal performance measurements](docs/METAL-GI-OPTIMIZATION.md).
The [Xcode capture follow-up](docs/METAL-GI-XCODE-FOLLOWUP.md) records subsequent
reflection scheduling changes, GPU counters and the limits of measured gains.
MegaLights/ReSTIR remain later work.

## Current status

The active path is **standard G-buffer deferred rendering with Surfel GI**.
The visibility-ID pass and geometry material replay have been removed. A single
opaque/cutout material raster pass writes albedo/metallic, shading normal/roughness,
emission, material response, geometric normal/instance metadata and motion.
Native fullscreen clustered lighting resolves those attributes.

GI uses original scene triangles, persistent surfels, Vulkan/Metal hardware ray queries
or a compute BVH fallback, MSME integration and diffuse multibounce. Independent
GGX reflection rays evaluate sun/local light, emission, sky and the diffuse surfel
cache at world-space hits. Reflection histories track roughness, normals, depth
and hit distance; primary materials supply Fresnel and metallic response.
Opaque BaseMaterial3D albedo textures are sampled at ray-hit UVs from 512x512
pages. Sponza defaults to a wet dielectric floor (roughness 0.18); `--dry-floor`
uses 0.85 and `--no-specular` isolates indirect diffuse.
Host scenes can use [native GI integration APIs](docs/SCENE-GI-INTEGRATION.md) for scoped geometry, authored sky and shader-material inputs.
See [run instructions](sponza/README.md) and
[implementation, validation and limits](docs/SURFEL-GI.md).

The optimized 1080p release on RTX 5070 Ti measures **157–159 FPS with full GI**
in the repeated static Sponza benchmark (previously 10.1 FPS). Compact overlap
lists, world-space allocation deduplication and rough reflection sample reuse
remove the main costs. See the [optimization report](docs/GI-OPTIMIZATION-2026-09-17.md)
for GPU-pass timings, full-rate quality comparisons, the extended moving test,
builds and visual validation. These results apply to the documented hardware and workload.
The subsequent [stationary GI smoothness fix](docs/GI-SMOOTHNESS-2026-09-17.md)
adds geometric diffuse filtering and improves cache convergence. Its static
1080p release repeat measures 129 FPS on the same GPU; the earlier timing above
is historical. Spatial blotches and temporal flicker are now checked separately.

Historical proxy/SH and Mac non-GI measurements remain as historical records.
They do not validate this implementation or establish a performance improvement.

The current [GIBS-based diffuse revision](docs/SURFEL-GIBS-REVIEW-2026-09-17.md)
restores cache-based diffuse in every mode, shares incoming irradiance before
MSME, guides rays and enforces a variance-weighted global primary-ray budget.
The optional [NRD adapter](docs/NRD.md) now also processes full-resolution cache
diffuse with RELAX_DIFFUSE, replacing the original screen reconstruction and
temporal filter. It never replaces diffuse GI with screen-pixel ray tracing.
NRD defaults to off. The [linear diffuse review](docs/DIFFUSE-VALUES-2026-09-18.md)
records the subsequent cache-sharing revision using direct irradiance values.
The [cold-cache convergence follow-up](docs/DIFFUSE-CONVERGENCE-2026-09-18.md)
fixes false startup relighting and repeated history weighting, with early-frame
raw-diffuse comparisons and explicit remaining startup noise.
See the [cache/NRD comparison](docs/NRD-CACHE-DIFFUSE-2026-09-18.md) for measured
benefits, regressions and cost. NRD has its own
license; its SDK source is not vendored here. The figures above are historical;
the new report separates diffuse-only measurements from the older full-GI runs.

The [reflection roughness review](docs/NRD-REFLECTIONS-2026-09-18.md) adds current
sun/sky specular measurements. Deferred now demodulates the sampled reflection
with its actual primary material and DFG, runs one RELAX_SPECULAR instance and
restores the material basis. Normal NRD uses full-rate two-ray reflections;
checkerboarding is an explicit option with documented quality loss. Sponza's
panel exposes floor roughness, indirect specular and NRD controls.
