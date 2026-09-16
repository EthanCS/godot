# Performance measurements — Apple M5 / Metal

The originally requested GI-on all-dynamic 1080p/60 FPS target **did not pass**.
The current delivery defaults GI off at the user’s request; GI work is deferred. Functional checks
passed separately. The deferred path improves high-light-count static workloads,
but is slower at 32 lights and near parity/slower at 128. Native dynamic GI is the
largest measured incremental cost in the default moving scene; this is established
by matched GI-off/on replays, not by unavailable GPU timestamps.

## Conditions and identity

- Apple M5 (Apple9), 32 GiB unified memory; macOS 26.6.2 (25G83), Metal 4.0.
- Same Godot 4.7.2 upstream and custom **editor** binary for each A/B pair;
  `platform=macos target=editor arch=arm64 vulkan=no metal=yes -j8`, no .NET.
  The release export template was built and graphically tested separately.
- 1920×1080 real window, VSync disabled, fixed exposure 1, no bloom. Both paths
  use the same 797 island surfaces, 53 materials and shared authored BRDF.
- Static matrix: camera 0, simulation time 4, AA/AO off, 32/128/512/1024 lights,
  scattered/dense layouts, 0/8 local shadows, GI off/on. 192 runs: 32 conditions ×
  2 renderers × 3 repeats. At least 3 seconds plus 280 GI-on / 120 GI-off frames
  of warmup, then 4 seconds sampled per run. These are short controlled runs;
  per-run ranges and raw frame sequences are retained.
- Final static spot check: 128 scattered lights, 8 shadows, GI on, AA/AO off;
  at least 5 seconds and 320 frames warmup, then 4 seconds measured, 3 repeats.
- Dynamic: default 128 lights/8 local shadows, 12 emissive rigid bodies and four
  occluders, camera/TOD/lights/emission running together, TAA and XeGTAO on.
  Three 30-second replays for each renderer and GI off/on: 12 runs. Warmup is at
  least 5 seconds and 320 GI-on / 120 GI-off frames. All animation clocks reset to
  zero when measurement begins; frames then follow common wall-clock time.
  This timed sample covers the first 30 seconds, not the complete day/night cycle.
  The separate 305-second capture tour covers the longer visual cycle.
- GI quality is identical between renderer pairs: 1 stationary ray/frame, 256-ray
  confidence cap; changing lighting uses at least 4 rays/frame and bounded
  history. Both use software BVH. TAA stationary receivers continue tracing fresh
  samples instead of freezing a screen texel. No hidden quality reduction.
- Directional shadow texture and positional shadow atlas are both 4096. Cluster
  capacity is 2048. Static runs verify the exact 3:1 omni/spot counts and shadow
  counts. The timed 30-second dynamic replay uploaded all 128 lights and 8 shadows
  in all 12,268 profile snapshots; longer tours can legitimately frustum-cull some
  lights. No light overflow was recorded in any of 88,753 audited snapshots.
- Runs are serial; A/B order alternates by repetition. No screenshot or buffer
  readback occurs during timing, and no other Kiln GPU check/build runs alongside.
  Lightweight report editing continues on the host. CPU profiling is enabled
  equally. OS scheduling and thermals are not controlled laboratory variables.

The full static matrix used frozen source `f4e93aac42f704dcf9ea8d1983b363e55e0c6ea1`.
The final static check and dynamic replay use engine source
`06843311c6520e289017422c7a6dbbd4d0798143`, after material/mesh lifecycle and TAA
history fixes. The old matrix is not relabeled as the final build. Its fixed,
unjittered workload is separately spot-checked by the final six runs. Exact binary,
PCK, source and working-diff hashes are in each compact JSON baseline; the final
working diff at dispatch contains report/evidence documentation only. Delivery
`b3129706fd` subsequently fixes editor-help/export teardown and makes GI opt-in;
it changes no rendering algorithm. The measured `06843311c6` binary/PCK remain
frozen in `/tmp/kiln-06843311`. See validation-host.json for both identities.

## All-dynamic result

Values are the median of three per-run percentiles, in milliseconds. Frame-time
percentiles sample rendered frames; the plotted one-second bins give a separate
common-time view so slow sections do not disappear due to their lower frame count.

| Lights | Layout | Shadows | GI | Forward median / P95 / P99 | Deferred median / P95 / P99 | D/F | VRAM MiB F / D |
| ---: | --- | ---: | --- | --- | --- | ---: | --- |
| 128 | scattered | 8 | off | 17.342 / 20.497 / 21.431 | 18.057 / 21.204 / 22.085 | 1.041 | 1119.7 / 1168.0 |
| 128 | scattered | 8 | on | 70.099 / 110.656 / 121.004 | 73.230 / 115.536 / 127.051 | 1.045 | 1119.7 / 1168.0 |

Forward+: GI-off median 17.342 ms; GI-on 70.099 ms (14.3 FPS equivalent), an incremental 52.757 ms. This delta includes GI-dependent pipeline/resource behavior; it is not an isolated GPU dispatch measurement.

Deferred: GI-off median 18.057 ms; GI-on 73.230 ms (13.7 FPS equivalent), an incremental 55.173 ms. This delta includes GI-dependent pipeline/resource behavior; it is not an isolated GPU dispatch measurement.

The GI-on P95/P99 also exceed the 16.67 ms budget. The implementation therefore
has no stable-60-FPS claim on this hardware. Disabling GI is diagnostic isolation,
not a proposed substitute for the required default quality.

## Static scaling matrix

Median / P95 / P99 are milliseconds. D/F below 1 favors deferred. All original
slower cases are retained. GPU stage costs are N/A.

| Lights | Layout | Shadows | GI | Forward median / P95 / P99 | Deferred median / P95 / P99 | D/F | VRAM MiB F / D |
| ---: | --- | ---: | --- | --- | --- | ---: | --- |
| 32 | scattered | 0 | off | 5.020 / 6.193 / 6.626 | 5.556 / 6.722 / 7.066 | 1.107 | 296.3 / 360.8 |
| 32 | scattered | 0 | on | 8.769 / 10.430 / 10.934 | 9.574 / 10.824 / 11.227 | 1.092 | 1020.4 / 1076.8 |
| 32 | scattered | 8 | off | 5.058 / 6.202 / 6.702 | 5.595 / 6.656 / 7.053 | 1.106 | 341.4 / 405.9 |
| 32 | scattered | 8 | on | 8.802 / 10.262 / 10.981 | 9.563 / 10.828 / 11.156 | 1.086 | 1065.2 / 1121.6 |
| 32 | dense | 0 | off | 5.097 / 6.179 / 7.041 | 5.582 / 6.654 / 7.194 | 1.095 | 296.3 / 360.8 |
| 32 | dense | 0 | on | 8.779 / 10.416 / 10.848 | 9.570 / 10.893 / 11.297 | 1.090 | 1020.4 / 1076.8 |
| 32 | dense | 8 | off | 5.135 / 6.230 / 6.633 | 5.625 / 6.756 / 7.580 | 1.095 | 341.4 / 405.9 |
| 32 | dense | 8 | on | 8.814 / 10.516 / 10.969 | 9.552 / 10.896 / 11.330 | 1.084 | 1065.2 / 1121.6 |
| 128 | scattered | 0 | off | 6.369 / 7.647 / 8.027 | 6.480 / 7.639 / 8.150 | 1.017 | 296.3 / 360.8 |
| 128 | scattered | 0 | on | 9.979 / 11.180 / 11.650 | 10.382 / 11.585 / 12.096 | 1.040 | 1020.4 / 1076.8 |
| 128 | scattered | 8 | off | 6.434 / 7.726 / 8.137 | 6.501 / 7.852 / 8.203 | 1.010 | 341.4 / 405.9 |
| 128 | scattered | 8 | on | 9.948 / 11.400 / 11.806 | 10.406 / 11.724 / 12.457 | 1.046 | 1065.2 / 1121.6 |
| 128 | dense | 0 | off | 7.112 / 8.715 / 9.119 | 7.042 / 8.502 / 8.840 | 0.990 | 296.3 / 360.8 |
| 128 | dense | 0 | on | 10.408 / 11.806 / 12.623 | 10.949 / 12.258 / 12.792 | 1.052 | 1020.4 / 1076.8 |
| 128 | dense | 8 | off | 7.221 / 8.705 / 9.318 | 7.044 / 8.523 / 9.144 | 0.975 | 341.4 / 405.9 |
| 128 | dense | 8 | on | 10.448 / 11.880 / 12.457 | 10.961 / 12.357 / 12.825 | 1.049 | 1065.2 / 1121.6 |
| 512 | scattered | 0 | off | 12.055 / 13.806 / 14.301 | 10.148 / 11.354 / 11.823 | 0.842 | 296.3 / 360.8 |
| 512 | scattered | 0 | on | 15.434 / 17.887 / 18.818 | 14.115 / 16.048 / 16.730 | 0.915 | 1020.8 / 1077.2 |
| 512 | scattered | 8 | off | 12.115 / 13.726 / 14.274 | 10.166 / 11.402 / 11.874 | 0.839 | 341.4 / 405.9 |
| 512 | scattered | 8 | on | 15.453 / 17.786 / 18.976 | 14.061 / 16.220 / 16.813 | 0.910 | 1065.6 / 1122.0 |
| 512 | dense | 0 | off | 15.923 / 19.056 / 19.930 | 12.625 / 14.627 / 15.636 | 0.793 | 296.3 / 360.8 |
| 512 | dense | 0 | on | 18.605 / 21.971 / 23.517 | 16.698 / 18.941 / 19.892 | 0.898 | 1020.8 / 1077.2 |
| 512 | dense | 8 | off | 15.925 / 18.932 / 20.048 | 12.657 / 14.661 / 16.019 | 0.795 | 341.4 / 405.9 |
| 512 | dense | 8 | on | 18.735 / 22.321 / 22.880 | 16.585 / 18.922 / 19.980 | 0.885 | 1065.6 / 1122.0 |
| 1024 | scattered | 0 | off | 19.411 / 22.390 / 23.107 | 14.987 / 17.499 / 18.557 | 0.772 | 296.8 / 361.3 |
| 1024 | scattered | 0 | on | 22.798 / 25.767 / 26.135 | 18.805 / 20.623 / 21.342 | 0.825 | 1021.5 / 1077.9 |
| 1024 | scattered | 8 | off | 19.472 / 22.390 / 23.485 | 15.080 / 17.192 / 18.108 | 0.774 | 341.9 / 406.4 |
| 1024 | scattered | 8 | on | 22.812 / 25.405 / 26.839 | 18.569 / 20.891 / 21.503 | 0.814 | 1066.0 / 1122.4 |
| 1024 | dense | 0 | off | 28.649 / 35.506 / 37.826 | 20.664 / 23.919 / 25.649 | 0.721 | 296.8 / 361.3 |
| 1024 | dense | 0 | on | 31.587 / 37.740 / 39.539 | 26.826 / 29.342 / 30.469 | 0.849 | 1021.4 / 1077.8 |
| 1024 | dense | 8 | off | 30.206 / 36.221 / 37.966 | 20.743 / 24.495 / 25.806 | 0.687 | 341.9 / 406.4 |
| 1024 | dense | 8 | on | 32.517 / 37.963 / 41.130 | 24.488 / 27.951 / 30.240 | 0.753 | 1066.0 / 1122.4 |

At 1024 densely overlapping lights with eight local shadows and GI off,
frame time falls from 30.206 to 20.743 ms (31.3%); with converged GI it falls from
32.517 to 24.488 ms (24.7%). At 32 scattered lights/eight shadows/GI off, deferred
is 10.6% slower. Six MRT writes/readback traffic and the fullscreen material pass
have a fixed cost; the matrix is consistent with that cost becoming worthwhile
only as direct-light overlap increases. This is an interpretation of frame times,
not a separately measured bandwidth result.

## Final-build static spot check

| Lights | Layout | Shadows | GI | Forward median / P95 / P99 | Deferred median / P95 / P99 | D/F | VRAM MiB F / D |
| ---: | --- | ---: | --- | --- | --- | ---: | --- |
| 128 | scattered | 8 | on | 9.553 / 10.481 / 11.047 | 10.085 / 10.648 / 11.368 | 1.056 | 1065.2 / 1121.6 |

## Available stage timing

GPU timestamp results on this Metal backend were zero/unavailable and are stored
as `null` / N/A, never interpreted as zero execution cost. CPU numbers below are
median submission intervals between adjacent renderer profile markers, then
median across three runs, for the **dynamic GI-on** condition. These intervals
exclude uninstrumented scene-script/world-update work and cannot be summed to
predict frame time. Full labelled profiles are retained in raw run.json files.

| Stage | Forward+ CPU ms | Deferred CPU ms | GPU ms |
| --- | ---: | ---: | --- |

| Depth prepass | 0.1460 | 0.1660 | N/A |
| Shadow submission | 0.3225 | 0.3000 | N/A |
| Opaque material / G-buffer | 0.0900 | 0.0810 | N/A |
| Native GI (includes AO) | 0.0620 | 0.0570 | N/A |
| Deferred direct light | N/A | 0.0080 | N/A |
| Transparent | 0.0050 | 0.0060 | N/A |
| Tonemap | 0.0070 | 0.0080 | N/A |
| TAA | 0.0080 | 0.0080 | N/A |

## Cost and next work

The paired dynamic GI-off/on increase is much larger than the default 128-light
forward/deferred difference. Continuously changing lighting invalidates radiance
cache epochs, uses at least four rays per half-resolution receiver, and invokes
static/dynamic software BVH visibility and analytic/emitter sampling. The static
BVH itself stays at geometry version 1; moving lamps do not rebuild the island.
The evidence points to GI transport/update work as the first optimization target,
but the absence of GPU timestamps prevents an exact split among ray traversal,
filtering, publication and synchronization.

Next steps are to obtain reliable Metal GPU stage profiling, add regional rather
than global radiance invalidation for moving local lights, and improve software
BVH/instance traversal and sampling reuse under continuous TOD. Those changes
must retain the existing rejection/offscreen/flash tests and equal-quality A/B.
Hardware traversal is a later capability-dependent option; the actual RD reports
ray queries and ray-tracing pipelines unsupported here. Neither MegaLights nor
ReSTIR has been implemented or used to claim a result.

The static GI-off deferred allocation is approximately 64.5 MiB above Forward+;
with GI it is approximately 56.4 MiB higher. The G-buffer alone has 32 bytes per
pixel before depth (63.3 MiB at 1080p). Reported video-memory values are the engine
monitor's resource estimate on unified memory, not measured physical residency or
bandwidth. Per-condition allocations are in the tables.

## Raw evidence and reproduction

- `/tmp/kiln-matrix-final`: 192 engine logs, raw frame arrays and CPU profiles,
  per-run CSV, analysis JSON, scaling.png and scaling.pdf.
- `/tmp/kiln-final-quick`: six final-build no-AA runs, CSV and analysis JSON.
- `/tmp/kiln-dynamic-accepted`: twelve final-build dynamic runs, CSV and analysis,
  timeline.png and timeline.pdf; no timing captures.
- `/tmp/kiln-final-tour`: the separate 305.10-second visual tour, 146 captures;
  source predates the final TAA fix and timing is contaminated by readbacks.
- `/tmp/kiln-validation-accepted`: final GPU correctness suite and exact results.
- `/tmp/kiln-reference-diff-final`: five source/native/error image sets and metrics.
- `/tmp/kiln-final-export/KilnIsland.app`: final self-contained release package;
  run outside the repository. GI off by default; unsigned local development artifact.

The failed release-template `--main-pack` launch in `/tmp/kiln-dynamic-final`
produced no timing run: this template disables path overrides. All reported
performance runs consistently use the editor binary, which supports the explicit
PCK. The exported app locates its bundled PCK normally and passed standalone
rendering checks. Import/export additionally reject logged errors even if the
engine crash handler returns status zero; late editor-help callbacks were fixed
after this check caught an early CLI shutdown failure. Temporary paths may expire; compact baseline JSON stays in git.

```
python3 kiln/tools/benchmark.py --output /tmp/new-matrix
python3 kiln/tools/benchmark.py --dynamic --duration=35 --warmup=5 --output /tmp/new-dynamic
python3 kiln/tools/analyze_benchmark.py /tmp/new-matrix --require-complete
python3 kiln/tools/analyze_benchmark.py /tmp/new-dynamic --require-complete
python3 kiln/tools/plot_benchmark.py /tmp/new-matrix --dynamic /tmp/new-dynamic
```

The first command uses the tool's documented longer default sample duration;
pass `--duration=7 --warmup=3` to reproduce the recorded four-second static sample.
Plotting requires NumPy and Matplotlib. Captures and benchmarks must run serially.
