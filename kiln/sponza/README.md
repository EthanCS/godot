# Sponza GI benchmark

The active GI acceptance scene is Sponza. GI is enabled by default. The old island
project is retained as historical material, not used by this scene.

## Run

From the repository root:

```powershell
python kiln/tools/fetch_sponza.py
bin/godot.windows.editor.x86_64.mono.console.exe --headless --editor --import --path kiln/sponza
kiln/sponza/run.ps1
```

The headless command only imports assets. All rendering checks use a window on a
real GPU. On macOS, build the changed engine and run its native executable with
`--path kiln/sponza --rendering-driver metal --rendering-method kiln_deferred`.
The new implementation has **not** been tested on macOS.

Space pauses TOD, lights and emitters. C toggles the camera orbit. Hold RMB and use
WASD/QE to fly; G toggles GI; F1 shows indirect illumination. Simulation advances
by exactly 1/60 second per rendered frame so renderer comparisons replay identical
inputs. The default scene is **sun and sky only**, with a time-of-day slider. Moving the
slider pauses the clock; Space resumes from the chosen time. The checkbox or
`run.ps1 -MultiLight` enables the additional eight omni lights, four spot lights,
four local shadows and three animated emissive meshes. Those props are hidden in
sun/sky mode. `--suite` continues to test the full multi-light workload.

CLI options after `--`: `--still`, `--no-gi`, `--no-aa`, `--rays=2`,
`--multi-light`, `--hour=12`, `--software` (force fallback), `--hardware`,
`--proxy-ratio=0.12`, `--gi-divisor=4` (balanced; use 2 for quality), `--size=1280x720`, `--frames=600`, `--output=<temp-directory>`.
`--benchmark` records frame intervals after 60 warmup frames, with no captures or
GPU buffer readbacks. Both renderers must use identical options. These are whole
frame timings, not per-stage GPU timestamps. `run.ps1 -ForwardPlus` uses Forward+.

Imported OBJ/glTF/FBX scene meshes contain an inspectable `KilnGIProxy` resource
in `mesh.get_meta("kiln_gi_proxy")`, including `proxy_mesh`. It is generated during
import and persists in the imported resource and exported game. Source color,
texture, emission and material replacement update the solid-color proxy; there
is no manual generation or material-rebuild step. Instance overrides update GI
transport independently so they cannot recolor another instance's shared proxy.

## Sun/sky TOD gallery and video

```powershell
$engine = './bin/godot.windows.editor.x86_64.mono.console.exe'
& $engine --path kiln/sponza --rendering-driver vulkan --rendering-method kiln_deferred -- --tod-suite "--output=$env:TEMP/kiln-sponza-tod-hardware"
python kiln/tools/check_tod.py "$env:TEMP/kiln-sponza-tod-hardware"
& $engine --path kiln/sponza --rendering-driver vulkan --rendering-method kiln_deferred -- --tod-video "--output=$env:TEMP/kiln-sponza-tod-video"
```

TOD captures use a fixed camera at 06:30, 09:00, 12:00, 17:30 and 21:00. Each has
GI on/off and indirect-only images, followed by a rotating-camera daylight cycle.
The video writes 480 PNG frames for a continuous 24-hour cycle. All local lights
and emissive props remain hidden. Readbacks are excluded from performance claims.
Use `python kiln/tools/build_tod_gallery.py <capture-directory> <output-directory>`
to build an HTML gallery with time selection, GI on/off wipe and indirect view.
An optional `tod.mp4` in that directory is displayed as the continuous cycle.
The `check_tod.py --compare <other-backend-directory>` option checks equivalent
hardware/software images in addition to isolated diffuse transport and noise.

## Automated GPU checks

```powershell
$engine = './bin/godot.windows.editor.x86_64.mono.console.exe'
& $engine --path kiln/sponza --rendering-driver vulkan --rendering-method kiln_deferred -- --suite --size=960x540 "--output=$env:TEMP/kiln-sponza-validation"
python kiln/tools/check_sponza.py "$env:TEMP/kiln-sponza-validation"
& $engine --path kiln/sponza --rendering-driver vulkan --rendering-method kiln_deferred res://proxy_lifecycle.tscn -- "--output=$env:TEMP/kiln-proxy-lifecycle"
```

For a reproducible combined run on either host, use:

```powershell
python kiln/tools/validate_sponza.py --engine $engine --driver vulkan --output "$env:TEMP/kiln-sponza-validation"
```

The suite isolates darkness, sun/sky, TOD, local lights and material emission. It
captures camera rotation and moving emitters, tests turn-off decay and measures
stationary temporal noise and post-motion settling. Raw GI readbacks ensure a
visible emissive object or direct light cannot masquerade as indirect transport.
Every diagnostic capture compares compute-built BVH bounds to the CPU reference.
Lifecycle checks cover live texture pixel updates, material replacement, static
transforms, in-place mesh edits, visibility changes and unused cache release.

The importer fixture runs **on a GPU**, without a GI node, to verify persisted
OBJ/glTF proxies, color/texture/emission edits, material replacement and reload:

```powershell
& $engine --path kiln/sponza --rendering-driver vulkan --script res://tests/import_proxy.gd
```

See [GI implementation and validation](../docs/GI-PROXY.md) for the exact scope.

## Asset provenance

Source: [McGuire Computer Graphics Archive](https://casual-effects.com/g3d/data10/index.html),
Crytek Sponza by Frank Meinl, modifications by Morgan McGuire (2011).
The fetch script pins the archive SHA-256 and preserves `assets/copyright.txt`.
The notice explicitly describes donation for radiosity and use with multiple
renderers; it does not contain a full CC license. No broader license is asserted.
Downloaded assets are ignored by Git and no assets have been published.

Conversion merges OBJ faces by material to avoid Godot's surface-count limit;
vertex positions, UVs and normals are preserved. Historical OBJ/PBR differences
(including bump-map interpretation and opacity-map support) remain a limitation
of this reference import. It is not the Intel remaster or Khronos glTF version.
