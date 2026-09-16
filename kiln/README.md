# Kiln rendering pipeline

Engine development branch based on Godot **4.7.2-stable**, pinned in
[`upstream.json`](upstream.json). The upstream README and license remain applicable.

## Phase 1

Implement an engine-level hybrid deferred rendering path and integrate Kiln GI.
Opaque and cutout geometry use a real G-buffer and clustered deferred lighting;
transparency, additive effects, and refraction use an explicit forward stage.
Retain a working Forward+ path for controlled comparisons.

The standalone island demo must include an automatic camera, continuous time of
day, moving emissive surfaces, moving occluders, and moving point/spot lights.
Default load: 128 local lights, including 8 shadowed lights, plus the sun;
stress presets: 32/128/512/1024 lights and 0/8/16/32 local shadow lights.
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
hardware; it is an optimization target, not a measured result. Hardware ray
tracing and MegaLights/ReSTIR are later work, not Phase 1 dependencies.

## Current status

The current delivery focuses on the non-GI renderer, per the user’s scope update.
GI starts off in the demo and is opt-in with G / `--gi`; further GI work is deferred.

A native opaque G-buffer, clustered deferred lighting and Kiln GI now run on
macOS Metal. See [implementation and validation status](docs/STATUS.md),
[pipeline contract](docs/PIPELINE.md), [upstream integration map](docs/CHANGESET.md),
[performance measurements](docs/PERFORMANCE.md) and [demo instructions](demo/README.md).
Phase 1 acceptance remains distinct from implementation. Captures and full logs
are temporary local artifacts. Source asset/shader redistribution is not cleared.
