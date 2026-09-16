# Kiln rendering pipeline

Engine development branch based on Godot **4.7.2-stable**, pinned in
[`upstream.json`](upstream.json). The upstream README and license remain applicable.

## Phase 1

Implement an engine-level hybrid deferred rendering path and integrate Kiln GI.
Opaque and cutout geometry use a real G-buffer and clustered deferred lighting;
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
ray queries are implemented with a compute software-BVH fallback. Metal hardware
ray tracing and MegaLights/ReSTIR remain later work.

## Current status

GI uses automatically imported, persistent geometry/material proxies, live material
updates, Vulkan hardware ray queries, a compute software-BVH fallback and temporal
denoising. **Sponza is
the active GI benchmark**, with GI enabled by default. Start with
[the Sponza instructions](sponza/README.md) and
[the implementation/validation record](docs/GI-PROXY.md).

The prior non-GI delivery and its macOS evidence remain in [STATUS.md](docs/STATUS.md).
They do not validate the new GI on Mac. The existing native G-buffer, deferred
lighting, forward transparency and other renderers are preserved.
