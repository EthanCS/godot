# Native scene GI integration — 2026-09-17

## Implemented

`KilnGIWorld.set_capture_roots()` selects scene subtrees without a project-side geometry copy or compositor. Empty roots retain the previous parent-subtree behavior. Duplicate/overlapping roots are captured once; nested viewports are excluded from both geometry and local-light traversal. Rigid animation belongs under `kiln_dynamic` metadata; putting animated geometry in a large static root causes that static BVH to rebuild.

`set_ao_quality()` exposes independent XeGTAO settings. `set_sky_parameters()` forwards the same procedural sky halo, saturation and cloud inputs used by a host scene. Default values preserve the standalone demo's existing sky.

The explicit `kiln_uniform_transport` ShaderMaterial contract now supports optional albedo textures, UV transforms/world-XZ mapping, vertex reflectance and average emission-texture radiance. Shader materials still opt in. This is declarative ray-hit material input, not project-provided GI code. Materials without UVs retain their texture-average fallback.

Native statistics expose render dimensions, rendered frames, camera-motion state and geometry versions. GPU captures remain the source of surfel age/sample evidence; CPU stationary sample counters do not prove convergence.

## Built

macOS arm64 Metal .NET editor and template_release compiled. Mono glue/assemblies regenerated; the sibling OceanCastle Client compiled in Debug and ExportRelease against those bindings. No Windows/Linux/Vulkan/Mobile/Compatibility or packaged export validation is claimed here.

## Real GPU validation

Apple M5, macOS 26.6.2, Metal 4.0:

- Deferred and Forward+ each pass 38 isolated integration checks with Metal API validation: TAA history retention, diffuse energy, textured project materials, explicit roots, dynamic metadata, nested viewport/light isolation, motion, lights-off and odd dimensions.
- OceanCastle's full rendering smoke passes with Metal API validation, including menu/port/interiors, sea levels, return transitions and tutorial. Flying gulls update their native dynamic geometry while the static reef BVH version remains unchanged.
- 797 project material surfaces / 53 materials pass their contract test. Open-sky, zero-input, environment override and enclosed-sky checks pass. Port, tavern and sea screenshots were inspected.

Logs/captures are in temporary storage. The sibling game's `Doc/engine-gi-migration-2026-09-17.md` records the main-island timing protocol, raw observations and performance limits separately.

## Unsupported

Ray geometry remains rigid MeshInstance3D only: no skinned/deforming/blend-shape or MultiMesh capture, no blended/cutout ray visibility, and no arbitrary procedural ray-hit shader execution. Authored raster materials remain visible. Procedural scene surfaces explicitly declare average ray reflectance; emission textures are averaged. This integration does not claim identical ray/raster evaluation for those unsupported cases.

## Main synchronization

Before publishing, this integration was rebased onto `9db58bbb45` (stratified sampling and diffuse filtering). Both macOS arm64 .NET targets were rebuilt successfully, and the Deferred/Forward+ integration tests each passed all 38 checks again with Metal API validation. Logs use the `push-main-` prefix in the temporary evidence directory. The game timing and full-scene results above were collected before that upstream filter change; no post-rebase FPS result is claimed.
