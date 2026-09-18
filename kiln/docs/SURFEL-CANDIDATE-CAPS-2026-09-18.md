# Bounded surfel candidate searches

Date: 2026-09-18

This follow-up bounds the two high-frequency surfel-cache gathers using the
contributor limits used by Webgiya:

- secondary ray-hit lighting accumulates at most 32 usable contributors;
- the screen-space surfel resolve accumulates at most 64 usable contributors;
- surfel generation's allocation-coverage lookup remains exhaustive.

The limit is shared across the three radius levels. The shared GLSL source and
the optional handwritten Metal reflection path use the same 32-contributor hit
limit.

## Implemented

The limits live in `surfel_data.inc` and are selected by compile stage. Generated
`kiln_gi.glsl` was refreshed from the source fragments. `surfel_specular.metal`
has the equivalent bound for the optional handwritten Metal implementation.

The first version counted the first entries in Kiln's 18-bit hashed bucket.
Unlike Webgiya's camera-centered exact grid, that bucket can contain links from
unrelated world cells. Atomic scatter also changes their order between frames.
On the upper gallery this let collision entries consume all 64 slots, producing
the reported dark, flashing blocks. The corrected link keeps the surfel ID in
its low 18 bits and a full-hash fingerprint in its upper 14 bits. The link stays
32 bits wide while lookup applies to the intended spatial cell rather than to an
aliased hash bucket.

An exact-cell entry now consumes the 32/64 contributor budget only after radius,
normal, receiver-plane and lighting-confidence tests pass. This prevents an
opposite wall or unresolved newborn from exhausting the result set. It also
means 32/64 is an accumulation bound rather than a strict bound on inexpensive
link and geometry probes; that distinction is recorded under remaining risks.

The exact corridor reproduction exposed a second, larger issue: Kiln evaluated
the cache before allocation, so a newly visible 8x8 tile was black for at least
one frame and filled in block by block. Webgiya allocates, rebuilds its grid,
integrates newborn surfels and resolves them in the same frame. Kiln now follows
that order. Stable frames find holes against the complete previous grid and
perform one update/rebuild before tracing. Initialization and geometry changes
build a safe current grid first. A camera translation above 0.20 m or rotation
above 4 degrees runs one additional coverage repair pass; ordinary walking and
turning retain the single-rebuild path.

## Reference implementations checked

W298/SurfelGI, a Falcor implementation based on GIBS, exhaustively evaluates the
current cell for screen generation/evaluation. A 16x16 tile elects its minimum-
coverage pixel, placement is probabilistic, the defaults allow 150,000 total and
1,024 per cell, and newborn radiance is initialized from the tile's existing
gather. Its default 240-frame blend delay hides immature estimates. Its optional
bounded search applies to secondary continuation, not the screen resolve, and is
disabled by default.

Webgiya uses a camera-centered six/eight-level grid, gates continued allocation
around 32 entries per cell, looks at up to 128 entries while finding missing
coverage, resolves the first 64 entries, and looks up the first 32 at secondary
hits. It allocates before rebuilding/integrating/resolving, seeds newborns from
nearby irradiance, and fades them over four frames toward a 32-sample confidence
target. Kiln retains its world hash, exact-cell fingerprint and SurfelPlus/MSME
estimator rather than copying Webgiya's complete data structure.

## Compiled and shader-validated

- All generated Kiln shader variants passed `check_surfel_shaders.py`, including
  glslang compilation and SPIR-V validation.
- The macOS arm64 editor target built successfully from Godot commit
  `2a4985787db6a2c88368c56971cb9cea243f7695`.
- Built binary SHA-256:
  `16ffb78d5b43dc150ac782c116b42ab353492d6ecab2cb55173c92d8aedaac8b`.
- A real Metal run selected and compiled the optional handwritten MSL reflection
  implementation successfully.

The release template was not rebuilt for this follow-up.

## Real-GPU performance check

Hardware was an Apple M5 using Metal hardware ray queries. The workload was the
still Sponza scene at 1920x1080, two diffuse rays, two checkerboard reflection
rays, 300 frames, with the last 120 reported. These are whole-frame editor
measurements, not isolated GPU pass timestamps.

| Workload | Unbounded baseline | Initial unsafe cap mean | Corrected tagged cap | Corrected change |
| --- | ---: | ---: | ---: | ---: |
| Full GI | 52.209 ms | 46.015 ms | 45.466 ms | -12.9% |
| Diffuse only | 34.135 ms | 31.514 ms | 31.535 ms | -7.6% |
| GI off control | 9.264 ms | 8.669 ms | not repeated | control only |

The corrected full-GI run changes from 19.15 to 21.99 FPS; diffuse-only changes
from 29.30 to 31.71 FPS. The earlier GI-off control moved by 0.595 ms between
builds, so the table is a short whole-frame check rather than an isolated-pass
claim. Importantly, filtering bucket aliases did not give back the cap's gain.

The two capped runs are close, but this is still a short sequential A/B. Desktop
load, compilation state and temperature were not laboratory-controlled. The
result shows a useful improvement, not achievement of the 1080p/60 target.

Raw temporary evidence:

- unbounded baseline: `/tmp/kiln-webgiya-compare.QYW7Le`;
- capped run 1: `/tmp/kiln-webgiya-cap-benchmark.HVCAj5`;
- capped run 2: `/tmp/kiln-webgiya-cap-repeat.tglPdS`;
- corrected cap and density sweep: `/tmp/kiln-density-benchmark.Qt1zbt`;
- final full-GI run: `/tmp/kiln-final-full-bench.WzEBMd`;
- final static diffuse run: `/tmp/kiln-webgiya-order-static.J0dskc`;
- final smooth-motion diffuse run: `/tmp/kiln-webgiya-order-moving.mTHKx3`.

After fixing same-frame admission, a final 1920x1080 Apple M5 Metal check measured
49.228 ms for static full GI, 32.786 ms for static diffuse-only, and 35.991 ms for
diffuse-only during deterministic smooth camera/TOD motion. Compared with the
older unbounded measurements above those static values are 5.7% and 4.0% lower,
but this is not a contemporaneous A/B and should not be read as an isolated-pass
speedup. Correct same-frame admission returned part of the initial unsafe cap's
gain. A repair pass on every moving frame would cost 40.832 ms, so the final build
runs it only for large disocclusions; a third fill pass measured 43.500 ms and
was rejected.

## Surfel-density experiment

The projected surfel target diameter is now exposed for controlled comparisons
as `rendering/kiln/surfel_target_diameter_pixels` and the Sponza option
`--surfel-diameter`. Its default remains 20 pixels.

| Target diameter | Upper-gallery alive surfels | Full GI | Diffuse only | Composed normalized MAE vs 20 px |
| --- | ---: | ---: | ---: | ---: |
| 20 px | 19,963 | 45.466 ms | 31.535 ms | reference |
| 24 px | 16,464 (-17.5%) | 45.486 ms | 31.405 ms | 0.53% |
| 28 px | 14,044 (-29.7%) | 45.200 ms | 31.563 ms | 0.77% |

Reducing the count did not produce a repeatable frame-time improvement. The
fixed adaptive ray budget redistributes work over fewer surfels, while the
broader support introduces a small image difference. Therefore the experiment
does not justify changing the 20-pixel default.

## Visually verified

The initial ground-level and settled upper-floor checks were insufficient. The
final regression first matures the normal ground-floor cache, then jumps to
`(-4, 5.2, 5.4)` and its mirrored corridor and starts recording immediately.
Before same-frame allocation, raw-diffuse dropout covered 90.96% of active pixels
and 89.04% of active 8x8 tiles; composed dropout covered 79.01% and 74.31%.

With same-frame allocation and one large-disocclusion repair, the final binary's
raw dropout fell to 3.33% of active pixels and 0.25% of tiles; composed dropout
fell to 3.40% and 0.05%. The remaining events are concentrated in the first
teleport frame around
new silhouettes, not the former large tile field. Frames 0, 1, 3, 7 and 119 were
inspected. A separate 120-frame gradual translation/turn sequence was inspected
at frames 0, 30, 60, 90 and 119 and showed no large flashing black blocks.
`check_upstairs_stability.py` passes its teleport thresholds (raw below 1% of
tiles, composed below 0.2%). Evidence is in
`/tmp/kiln-final3-corridor.ehfJnm` and
`/tmp/kiln-webgiya-order-motion-stability.pcxMro`.

The immediately preceding Metal editor ran the visible 960x540 Surfel capture
suite with Metal debug validation enabled. `check_surfel.py` passed all 115
checks; multibounce cache coverage was 0.999986. The final rebuild changes only
the filter's hard-coded default diameter to the equivalent pushed 20-pixel
setting and passed the visible corridor regression above. Separate visible
Forward+ and handwritten native MSL reflection hardware-ray-query smoke runs
reached frame 119 without engine or validation errors. Evidence is in
`/tmp/kiln-final2-surfel-suite.0BOfvW`,
`/tmp/kiln-final-forward-plus.XsVe7G`, and
`/tmp/kiln-final-native-msl.H5y8lN`.

## Not verified and remaining risks

- The exact numerical before capture includes tagged cell links but still used
  deferred-next-frame allocation; it is not a capture of the original unbounded
  build or the very first unsafe hash-bucket cap.
- The raw diagnostic does not reach mathematical zero dropout under a full-view
  teleport: 0.25% of active tiles cross the conservative temporal threshold.
  The production composed signal is 0.05%, and gradual motion was visually clean.
- The 32/64 limits bound accepted lighting contributors, not every link probe.
  Exact-cell fingerprint, radius and plane rejection may inspect more entries.
  An absolute memory-probe bound would require per-cell population control or a
  quality-ranked compact list, neither of which is implemented here.
- Windows/Vulkan/D3D12, export templates, software BVH fallback, local-light and
  emissive workloads, long motion/TOD runs and NRD were not tested in this quick
  follow-up. No performance claim is made for those configurations.
