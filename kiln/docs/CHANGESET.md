# Upstream integration map

Base: Godot 4.7.2-stable, `ed1daf0bf001b61586d9930840f2f1394092c079`.
All upstream history and the root license are retained. No changes are made to
the Mobile or Compatibility renderer implementations. Their platform execution
is outside the current Windows Vulkan verification; earlier Mac evidence is
retained separately in STATUS.md.

| Area | Principal files | Purpose |
| --- | --- | --- |
| Renderer selection | `main/main.cpp`, `renderer_compositor_rd.cpp`, `renderer_viewport.cpp`, `servers/rendering/rendering_device.cpp`, editor renderer names/themes | Register the selectable `kiln_deferred` path and its editor/device presentation |
| Editor teardown | `editor/doc/editor_help.cpp` | Ignore late help callbacks after CLI import/export destroys the editor; prevents worker resurrection and shutdown errors |
| Opaque integration | `forward_clustered/render_forward_clustered.{cpp,h}` | Allocate the six MRTs, render opaque/cutout geometry, schedule GI and clustered deferred shading, preserve the later forward alpha/post stages |
| Material compilation | `scene_shader_forward_clustered.{cpp,h}`, `shader_types.cpp` | G-buffer variants, `kiln_surface` response and explicit unsupported-feature diagnostics |
| Raster shaders | `scene_forward_clustered.glsl`, `scene_forward_clustered_inc.glsl`, `scene_forward_lights_inc.glsl` | Material outputs, shared authored BRDF, deferred direct lighting, diagnostics and native GI material response |
| Scene snapshot | `scene/3d/kiln_gi_world.{cpp,h}`, `scene/register_scene_types.cpp` | Native public node, explicit geometry/material admission, independent static/dynamic/light/history revisions |
| GI proxies | `scene/resources/3d/kiln_gi_proxy.{cpp,h}`, `scene/3d/kiln_gi_proxy.cpp`, `editor/import/3d/resource_importer_{obj,scene}.{cpp,h}`, `scene/resources/{mesh,material}.cpp` | Persistent import-time simplified meshes, source material change propagation and runtime proxy reuse |
| Hardware queries | `drivers/vulkan/rendering_device_driver_vulkan.cpp`, `servers/rendering/rendering_device.{cpp,h}` | Enable optional Vulkan ray queries, query-only TLAS instances and shader compilation without unsupported re-spirv ray-query optimization |
| GI runtime | `renderer_rd/kiln/kiln_gi.{cpp,h}`, `kiln_world.{cpp,h}`, SCsub entries | Thread-safe snapshots, GPU resources, dispatch, capability reporting, capture and lifecycle |
| GI algorithms | `renderer_rd/kiln/sources/`, generated `shaders/kiln_gi.glsl` | Shared hardware/software queries, compute BVH bounds, analytic/emissive transport, temporal SH, world cache, filtering, publication and XeGTAO |
| TAA velocity copy | `effects/copy_effects.{cpp,h}`, `effects/taa.cpp`, `shaders/effects/copy.glsl` | Use RG16F-compatible storage and internal size when copying velocity history |
| Workload reporting | `storage_rd/light_storage.{cpp,h}` | Actual uploaded light/shadow and overflow counts |
| Independent demo | `kiln/sponza/`, historical `kiln/demo/` | Sponza sun/sky TOD, optional multi-light workload, deterministic GPU checks; no source-game runtime dependency |
| Reproduction | `kiln/tools/`, `kiln/docs/` | Build/export, isolated source capture, GPU checks, serial A/B measurements, analysis and provenance |

Renderer paths in this table are under `servers/rendering/renderer_rd/` unless
fully qualified. Source shader fragments are authoritative; regenerate the
combined shader with `python3 kiln/tools/build_gi_shaders.py`.

Forward+ shares the native Kiln GI and authored material response for controlled
comparisons. Ordinary opaque StandardMaterial3D also receives native GI when a KilnGIWorld
is active. Scenes without a KilnGIWorld and the transparent pass retain upstream
behavior. Shared upstream culling and shadow infrastructure is reused by both
comparison paths; performance gains are not attributed to a new shadow algorithm.

Inspect the complete change against the immutable base with:

```
git diff ed1daf0bf001b61586d9930840f2f1394092c079 -- servers scene main editor
git log --oneline ed1daf0bf001b61586d9930840f2f1394092c079..HEAD
```
