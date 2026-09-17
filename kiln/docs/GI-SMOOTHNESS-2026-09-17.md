# Stationary diffuse GI smoothness — 2026-09-17

The stationary Sponza image had visible mottling on otherwise smooth receivers.
The previous whole-image temporal-noise check did not measure this spatial error.
This change targets diffuse irradiance reconstruction, with the same scene,
lighting, ray budgets, materials, TAA, AO and reflection settings.

## Implemented

- Stratify each diffuse ray batch over the cosine hemisphere, with per-batch
  random rotation and jitter. Ray counts remain 4–32 per updated surfel.
- Stop mixing an unfiltered ray batch back into the MSME result indefinitely.
  That second blend imposed a persistent noise floor and bypassed firefly
  suppression. The initial convergence budget uses a bounded running mean;
  later updates use MSME. Explicit lighting changes retain fast adaptation.
- Add four full-resolution diffuse filtering passes at strides 1, 2, 4 and 8,
  followed by the existing temporal resolve. Geometric normals, bilateral plane
  distance, bounded world-space support and radiance weights reject incompatible
  receivers. Filtering occurs before material composition; it does not blur
  albedo, normal maps, direct shadows or the final framebuffer.
- Preserve the raw cache gather for diagnostic view 25 and capture `raw.bin`.
  The spatial result is not fed back into recursive cache transport. Two RGBA16F
  scratch textures add approximately 31.6 MiB at 1920×1080 and follow viewport
  resource destruction/reconfiguration.
- Add a deterministic real-window test and a spatial residual check on a fixed
  planar floor patch. Still, changing TOD and moving-camera sequences are saved
  without HUD; no headless run is counted as visual verification.

## Compiled

Windows x86_64 MSVC Mono production/speed editor and release export template.
All 24 native compute variants plus the native Metal interface shader pass
glslang/SPIR-V validation. Imported SurfelPlus source hashes remain unchanged.
These checks do not compile or validate the new filter on Metal hardware.

## Real-GPU verification

Windows/Vulkan, NVIDIA RTX 5070 Ti. Raw captures, logs, image comparisons and
frame arrays are under `%TEMP%/kiln-gi-noise/`. The machine-readable record is
[gi-smoothness-2026-09-17.json](gi-smoothness-2026-09-17.json).

At 1280×720, identical camera/noon lighting and frame 240:

| Fixed floor region | Before | After |
| --- | ---: | ---: |
| Relative spatial residual standard deviation | 0.06063 | 0.02254 |
| Mean linear diffuse irradiance / π | 0.04354 | 0.04338 |
| Whole-image stationary display temporal standard deviation | 0.002023 | 0.001952 |

The spatial residual falls **62.8%**, while the patch mean changes **−0.36%**.
This is a descriptive high-pass metric on 29,221 planar floor pixels, not error
against a path-traced reference and not a scene-wide noise reduction claim.
Actual indirect-only and material-composed images were inspected separately:
floor/cloth mottling is reduced, and texture detail and architectural boundaries
remain visible. Some broad variation remains during initial convergence.

Combined editor acceptance at 960×540 passes 115 diffuse/G-buffer checks,
300 lighting/motion checks, 223 reflection checks and 25 TOD checks, plus geometry
import/serialization and resource lifecycle. This covers light turn-off,
material changes, moving geometry, GI toggles and odd-size resize. Accepted
runtime logs contain no engine errors or Vulkan validation errors.
Additional real-window captures verify compute-BVH rendering at 480×270
(quality 3, twelve diffuse rays per updated surfel), Forward+ at 960×540, and
the embedded release at 1920×1080 after 1,600 stationary frames. Their diffuse
checks pass (11, 9 and 12 respectively); captured images were inspected.

## Performance and limits

Spatial filtering costs GPU time. The previous 157–159 FPS result belongs to the
earlier implementation and does not describe this quality revision. The new
1080p embedded-release static repeat measures 7.733 ms / 129.31 FPS; Forward+
with the same GI reconstruction/settings measures 9.679 ms / 103.32 FPS.
The initial deferred run measures 8.621 ms with a 99.325 ms maximum and seven
intervals above 16.67 ms; this outlier run is retained in the record. There is no
claim of universally hitch-free 60 FPS. Timing uses 180 warmup and 300 measured
frames without validation layers or readbacks; captures are separate runs.
The extended 3,600-frame camera/TOD run measures 8.890 ms mean / 10.572 ms P95,
with a 99.372 ms maximum and 13 of 3,420 measured intervals above 16.67 ms.

The filter is an approximation and can soften small indirect-light gradients
on one continuous surface. Low-sample glossy reflections and their history
rejection are unchanged; they can still show grain. Mac/Metal, Linux, D3D12 and
other GPU vendors have not been visually validated for this revision. Existing
transport limitations in [SURFEL-GI.md](SURFEL-GI.md) remain.

## Reproduce

Run the following once before changing the engine, then again with `after` in
place of `before` after rebuilding:

```powershell
& ./bin/godot.windows.editor.x86_64.mono.console.exe --path kiln/sponza --rendering-driver vulkan --rendering-method kiln_deferred --script res://tests/gi_noise.gd -- --still --size=1280x720 "--output=$env:TEMP/kiln-gi-noise/before"
python kiln/tools/check_gi_noise.py "$env:TEMP/kiln-gi-noise/before" "$env:TEMP/kiln-gi-noise/after"
```

For normal interactive viewing, `kiln/sponza/run.ps1 -Still` uses the rebuilt
editor. F1 shows composed indirect light; diagnostic view 25 intentionally shows
the unfiltered cache gather.
