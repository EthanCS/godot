# Sponza Surfel GI benchmark

Indirect diffuse now always comes from the persistent surfel cache, with
variance-driven irradiance sharing before MSME. Optional **NRD 4.17.3** handles
cache diffuse through RELAX_DIFFUSE and the independent reflection signals;
enabling it never starts a screen diffuse ray tracer. **NRD is off by default**;
opt in with `--nrd`, `run.ps1 -NRD`, or the panel switch.
The current sun/sky diffuse audit and measured limits are in
[the GIBS review](../docs/SURFEL-GIBS-REVIEW-2026-09-17.md).
The subsequent [reflection review](../docs/NRD-REFLECTIONS-2026-09-18.md) covers
roughness 0.06/0.18/0.45/0.85 with full-rate specular and NRD.

Sponza is the active acceptance scene for standard G-buffer deferred rendering and
Surfel GI. GI uses original mesh triangles: no simplified mesh, proxy import
metadata or manual generation step is required. See
[implementation and verified scope](../docs/SURFEL-GI.md).

## Run

On macOS, rebuild the native arm64 Metal editor, then launch the scene:

~~~sh
bash kiln/tools/build_macos.sh
./kiln/sponza/run.command
~~~

You can also double-click `run.command` in Finder. It works from any working
directory and uses the rebuilt `bin/godot.macos.editor.arm64` engine.
Scene options are passed through, for example `./kiln/sponza/run.command --still`.
Use `./kiln/sponza/run.command --forward-plus --still` for Forward+.
For a fresh checkout, fetch assets with `python3 kiln/tools/fetch_sponza.py`, then
import them with `bin/godot.macos.editor.arm64 --headless --editor --import --path kiln/sponza`.

From the repository root:

~~~powershell
python kiln/tools/fetch_sponza.py
bin/godot.windows.editor.x86_64.mono.console.exe --headless --editor --import --path kiln/sponza
kiln/sponza/run.ps1
~~~

Use `run.ps1 -Still` for paused noon, `-MultiLight` for the local-light workload,
`-TwoBounce` to disable recursive surfel-cache feedback, `-NoGI` for direct-only,
and `-ForwardPlus` for the existing Forward+ renderer. Headless is used only for
import/export, never as visual validation.

Space pauses the deterministic 1/60-second timeline. C toggles camera orbit;
hold RMB and use WASD/QE to fly. G toggles GI; B toggles recursive multibounce.
F1 shows linear indirect diffuse values before material, AO and denoising;
F2 shows G-buffer instance IDs, F3 geometric normals.
The debug selector also exposes indirect specular (28). The TOD
slider pauses the clock; Space resumes it. Default lighting is sun and sky only.
The optional workload adds eight omni lights, four spots, four local shadows,
three animated emissive meshes and moving occluders.

The original floor material is duplicated at runtime, preserving its texture.
Default roughness is 0.18 with metallic 0 and specular 0.5. `-DryFloor` / `--dry-floor`
sets roughness 0.85; `--roughness=0.06` gives a nearly smooth floor.
`-NoSpecular` / `--no-specular` isolates diffuse GI without disabling it.
`run.ps1 -Still -Roughness 0.06` shows a sharper floor. The right panel also has
a floor-roughness slider and independent indirect-specular/NRD switches.
`--specular-rays=1..8` sets the independent reflection budget (default 2).
With NRD enabled, normal rendering traces both samples at every valid pixel.
`--nrd-checkerboard` explicitly enables horizontal half-rate NRD input; it is
faster but has measured brightness/recovery differences. `--nrd-reference`
forces full rate. `--nrd-separate-specular` is the older two-denoiser comparison,
not the default deferred path. Forward+ retains the separate basis denoisers.
Without NRD, roughness >= 0.45 uses alternating half-rate samples only where adjacent
receivers agree in depth, normal and roughness. Wet/sharp reflections and edges
keep full-rate rays. `-FullSpecularRate` / `--full-specular-rate` disables this
reuse; the engine setting is `rendering/kiln/specular_checkerboard`.

CLI options after `--`: `--still`, `--no-gi`, `--no-aa`, `--rays=2`,
`--multi-light`, `--hour=12`, `--software`, `--hardware`,
`--surfel-two-bounce`, `--size=1280x720`, `--frames=600`,
`--output=<temp-directory>`.
Ray quality 1–8 supplies the nominal 4–32 rays per scheduled surfel. The GPU
redistributes these requests by variance and caps primary diffuse rays at
196,608 per frame (`--surfel-ray-budget=0` removes the cap). Shadow and
cache-miss continuation rays are additional work, included in measured time.
`--raw-cache` disables screen/temporal diffuse reconstruction.
`--no-irradiance-sharing` disables cache sharing for an independent comparison.
Use `run.ps1 -NoSpecular -Still -DebugView 25` to inspect raw diffuse values;
on macOS use `./kiln/sponza/run.command --no-specular --still --debug-view=25`.
Add `-NRD` / `--nrd` to opt into the denoiser. `--nrd-diffuse-iterations=2..5` selects RELAX A-trous iterations
(default 4). `--raw-cache` bypasses both post-filter paths.
The [cache/NRD comparison](../docs/NRD-CACHE-DIFFUSE-2026-09-18.md) documents
the input contract and measured quality/performance limitations.
The [linear diffuse review](../docs/DIFFUSE-VALUES-2026-09-18.md) records the
subsequent cache-sharing fix and direct numeric floor comparisons.
On macOS, use the arm64 engine with `--rendering-driver metal`; supported Apple
GPUs automatically select native Metal hardware ray queries. See
[Metal requirements and validation](../docs/METAL-RAY-QUERY.md). `--software`
still selects the compute BVH for diagnostic comparisons.
`--benchmark` records whole-frame intervals after warmup without readbacks.
Compare identical renderer options and quality; no performance result is implied.
For reproducible paired runs, use `kiln/tools/benchmark_sponza.py --engine <exe>
--output <temporary-directory>`; add `--embedded` for an exported executable.
Add `--diffuse-only` to restrict the cases to diffuse GI and matched GI-off runs.
Use `--compare-nrd` instead for paired diffuse NRD on/off runs and GI-off controls.
`--compare-reflections` sweeps four roughness values with full-rate two-ray
reflections, NRD on/off, diffuse/GI-off controls and the older NRD adapter.
This uses 180 warmup frames and 300 measured frames per short case at 1080p,
plus a 3600-frame moving regression to catch close-geometry allocation spikes.
The [optimization report](../docs/GI-OPTIMIZATION-2026-09-17.md) separates
full-rate, default-quality, GPU-pass and whole-frame measurements.

## Real-GPU validation

~~~powershell
$engine = './bin/godot.windows.editor.x86_64.mono.console.exe'
python kiln/tools/check_surfel_shaders.py
& $engine --path kiln/sponza --rendering-driver vulkan --rendering-method kiln_deferred --gpu-validation -- --surfel-suite --size=960x540 "--output=$env:TEMP/kiln-surfel-comparison"
python kiln/tools/check_surfel.py "$env:TEMP/kiln-surfel-comparison"
python kiln/tools/validate_sponza.py --engine $engine --driver vulkan --size 960x540 --output "$env:TEMP/kiln-surfel-validation"
~~~

The comparison checks multibounce, instance/geometric-normal diagnostics and odd-size
resize. Combined validation checks original OBJ/glTF imports and serialization,
isolated sun/sky/local/emissive transport, material changes without geometry
rebuilds, turn-off decay, motion coverage/settling, noise, resource lifecycle,
software/hardware selection and TOD. Diagnostic readbacks compare compute BVH
bounds and 2,048 hardware/software rays. Numerical checks and visual inspection
are separate; inspect captured color/indirect images too.

Static GI smoothness also has a dedicated test, `res://tests/gi_noise.gd`, and
`kiln/tools/check_gi_noise.py <before> <after>`. It measures spatial blotches on
an untextured floor patch in addition to frame-to-frame variation. See the
[smoothness report](../docs/GI-SMOOTHNESS-2026-09-17.md) for captures and scope.

For a TOD gallery, run `--tod-suite`, then `kiln/tools/check_tod.py <directory>`.
`--tod-video` captures 480 PNG frames of a 24-hour cycle.
`kiln/tools/build_tod_gallery.py <capture-directory> <output-directory>` builds
an HTML gallery; an optional `tod.mp4` is shown alongside still comparisons.

## Wet-floor reflection acceptance

~~~powershell
$engine = './bin/godot.windows.editor.x86_64.mono.console.exe'
& $engine --path kiln/sponza --rendering-driver vulkan --rendering-method kiln_deferred --gpu-validation -- --specular-suite --size=960x540 "--output=$env:TEMP/kiln-specular-check"
python kiln/tools/check_specular.py "$env:TEMP/kiln-specular-check"
~~~

The deterministic suite compares dry/wet and reflection on/off, isolates specular,
checks zero-F0 and metallic response, sweeps roughness 0.06/0.18/0.50/0.85, changes
emitter color, puts it outside the camera frustum, moves camera/source, tests
turn-off decay, disables GI and resizes to 961x541. Raw material attachments prove
roughness and primary-emitter absence. `validate_sponza.py` includes this suite.
Captures and logs remain in temporary storage. This is not a timing benchmark.

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

## Metal performance and ray-budget regression

`kiln/tools/benchmark_sponza.py` accepts `--driver metal`. Benchmark windows stay
on top, and the runner rejects stale rendered-frame counters so an occluded
window cannot produce a false speedup. Use production builds without validation
layers or diagnostic readbacks; keep GPU capture runs separate.

The real-window script `res://tests/specular_ray_counts.gd` switches 2 → 1 → 3 →
8 → 2 rays, checking finite, nonzero reflections at 481×271. Run it with
`--still --output=<temporary directory>` after the engine argument separator; add
`--software` to cover the compute-BVH pipeline cache. See the
[Metal optimization report](../docs/METAL-GI-OPTIMIZATION.md) for measurements.
