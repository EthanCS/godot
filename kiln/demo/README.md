# Dynamic island

Open this project with the custom engine built from this repository. An ordinary
Godot binary does not contain the renderer or `KilnGIWorld` node.

From the repository root:

```
./kiln/tools/build_macos.sh
bin/godot.macos.editor.arm64 --path kiln/demo --editor
bin/godot.macos.editor.arm64 --path kiln/demo
```

Default: automatic 120-second camera tour, 180-second TOD, 12 animated emissive
bodies, four moving occluders, 128 local lights (96 omni, 32 spot), eight local
shadow lights, XeGTAO and TAA. Fixed exposure, no bloom.

This delivery starts with **GI off** as requested. Existing experimental native
Kiln GI remains opt-in via **G** or `--gi`; GI performance/quality work is deferred.
The HUD reports the actual on/off state. Benchmarks explicitly select both modes.

| Control | Action |
| --- | --- |
| F1 | Expand/collapse diagnostics and select G-buffer/lighting/history views |
| Space | Pause/resume all timelines |
| C / T / L / E | Camera / TOD / light / emission animation |
| G / O / A / B | GI / AO / TAA / bloom |
| 1–4 | 32 / 128 / 512 / 1024 local lights |
| S | 0 / 8 / 16 / 32 shadow lights |
| D | Dense/scattered lights |
| P | Emission only → local lights only → TOD only → all |
| F | Cycle five original reference cameras and automatic tour |
| Tab | Free camera; hold right mouse to look; WASD/QE to move |
| Home | Reset automatic camera |
| [ / ] | Shift TOD by five simulation seconds |
| , / . | Slow (¼×) / fast (4×) TOD; press again for 1× |
| − / = | Decrease / increase local light range |
| R | Reload the scene |
| Esc | Quit |

Free-camera movement keys take precedence while free mode is active. Trajectories
and sample conditions are in `timeline.json`. `--time=4` freezes all animation at
a reproducible time; `--view=0` selects a reference camera. Use the same values
for renderer A/B. `--reference` restores the original source sky model, sun and
island-only composition. The source snapshot/provenance is in `../docs`.

```
# Fixed comparison; replace renderer with forward_plus for the other path.
bin/godot.macos.editor.arm64 --path kiln/demo --rendering-method kiln_deferred -- --time=4 --view=0 --no-aa --size=1920x1080 --duration=15 --report-dir=/tmp/kiln-fixed
# Diagnostic sequence (readbacks contaminate timing; never benchmark these).
bin/godot.macos.editor.arm64 --path kiln/demo -- --gi --preset=1 --debug=7 --capture-dir=/tmp/kiln-local --duration=12
# Native GI dynamic check and numerical evaluation.
bin/godot.macos.editor.arm64 --path kiln/demo res://tests/dynamic_lighting.tscn -- --output=/tmp/kiln-check
python3 kiln/tools/check_dynamic.py /tmp/kiln-check
# Opaque/cutout/normal-map and transparent/additive/refraction chart.
bin/godot.macos.editor.arm64 --path kiln/demo res://tests/material_contract.tscn
# Serial, three-repeat A/B matrix. No captures during measurements.
python3 kiln/tools/benchmark.py --output /tmp/kiln-benchmark
# Matched all-dynamic 30-second timelines, GI off/on, AO/TAA enabled, three repeats.
python3 kiln/tools/benchmark.py --dynamic --duration=35 --warmup=5 --output /tmp/kiln-dynamic-benchmark
```

`--diagnostics`, `--gi`, `--no-gi`, `--no-ao`, `--no-aa`, `--dense`, `--lights=N`, `--shadows=N`,
`--preset=0..3`, `--size=1001x703`, `--warmup=5`, `--profile` and `--no-hud`
`--tod-speed=4` and `--light-range=10` are supported. `--lifecycle` repeatedly resizes and switches cameras.
`--capture-at`, `--capture-interval` and `--buffer-at` control diagnostic readbacks.
Debug IDs: 0 lit, 1 albedo, 2 normal, 3 roughness, 4 emission, 5 material,
6 depth, 7 indirect, 8 AO, 9 motion, 10 direct, 11 cluster count, 12 history.

For a local self-contained macOS package:

```
./kiln/tools/build_macos.sh --templates
python3 kiln/tools/export_macos.py --output /tmp/kiln-export
/tmp/kiln-export/KilnIsland.app/Contents/MacOS/KilnIsland -- --duration=15
```

Run the complete serial GPU check suite with
`python3 kiln/tools/validate_macos.py --output /tmp/kiln-validation`.
Generate timing tables and raw CSV with
`python3 kiln/tools/analyze_benchmark.py /tmp/kiln-benchmark --require-complete`.

Additional checks use `res://tests/temporal.tscn` (then
`python3 kiln/tools/check_temporal.py /tmp/kiln-temporal`),
`res://tests/shadow_contact.tscn`, `res://tests/admission.tscn`, and
`--script res://tests/reload.gd`. `--headless --script res://tests/camera_path.gd`
checks only camera geometry clearance, not rendered output.

The local export is unsigned. Public redistribution of the imported source assets
and recovered shader material has not been cleared; local export is not a release.
See `../docs/STATUS.md` for actual checks and outstanding limitations.
