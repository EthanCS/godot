# Sponza performance baseline — 2026-09-17

This is the **pre-optimization** baseline, which did not meet 1080p/60 FPS.
See [subsequent optimization measurements](GI-OPTIMIZATION-2026-09-17.md) for the
current implementation. These measurements describe the tested Sponza workload,
not other scenes or platforms.

## Conditions

- Windows, Ryzen 9 9950X, RTX 5070 Ti, NVIDIA driver 610.88, Vulkan hardware ray queries.
- Existing production Mono release with embedded PCK; engine SHA-256: `5c1965a3441ea0525cdff753fc14f10b82bb3e56eb344b6a316ba64c8b69a3d8`.
- 1920x1080, TAA, wet floor roughness 0.18, sun/sky only. Diffuse quality 2; two reflection rays per pixel.
- VSync off. No validation layer, screenshots, raw GPU readbacks or GPU profiler during measurement.
- Same shadows and XeGTAO enabled in every case. GI-off retains the current GI integration/resolve housekeeping; it is not a GI-free engine build.
- 480 frames/run; discard the first 180 and measure 300 whole-frame wall intervals. FPS = 1000 / mean frame time. P95 is the 95th percentile frame time.
- Static view and moving view are different workloads. Moving cases share the exact same deterministic camera/sun timeline; they must be compared with one another.
- The two full-GI static configurations are repeated to check run-to-run drift. This is a short baseline, not a long thermal/endurance benchmark.

## Results

| Run | Mean FPS | Mean ms | Median ms | P95 ms |
| --- | ---: | ---: | ---: | ---: |
| Deferred, full GI | 10.12 | 98.84 | 98.07 | 102.39 |
| Deferred, diffuse GI only | 47.22 | 21.18 | 20.44 | 23.79 |
| Deferred, GI off | 463.34 | 2.16 | 2.14 | 2.33 |
| Forward+, same full GI | 7.85 | 127.45 | 126.22 | 132.76 |
| Forward+, GI off | 528.65 | 1.89 | 1.88 | 2.04 |
| Moving deferred, full GI | 12.16 | 82.24 | 94.07 | 115.66 |
| Moving deferred, diffuse only | 32.42 | 30.85 | 33.74 | 51.71 |
| Moving deferred, GI off | 464.04 | 2.15 | 2.15 | 2.32 |
| Deferred, full GI repeat | 10.12 | 98.77 | 98.13 | 101.97 |
| Forward+, full GI repeat | 7.84 | 127.48 | 126.04 | 133.36 |

## Interpretation

Turning reflection off reduces the first static deferred run by 77.66 ms/frame. This is an end-to-end toggle difference, not an isolated GPU-pass duration. The primary bottleneck is the current GI implementation, with a large reflection cost. Even diffuse-only GI misses 60 FPS in this test.

The current reflection shader traces at full resolution and performs a complete three-level Surfel neighborhood gather at each hit (up to 81 hashed cells, with linked-list traversal). This is a code-based candidate for the observed cost; a per-pass GPU profile is still required to divide tracing, cache lookup and denoising accurately. Roughness-aware ray allocation, resolution/sample reuse and cheaper cache lookup are the next profiling/optimization targets, not measured improvements.

Forward+/deferred results are end-to-end results for the current two implementations with matched scene and quality settings. They do not establish a universal renderer speedup or a comparison against the retired visibility-buffer version.

## Raw evidence

Raw frame arrays, exact configuration, process logs and summary: `C:\Users\Administrator\AppData\Local\Temp\kiln-deferred-performance`. Summary is also retained in [performance-2026-09-17.json](performance-2026-09-17.json). All measured runs exited successfully; validation layers were checked absent and logs were checked for engine errors.

One-run reproduction (change `--rendering-method`, `--no-specular`, `--no-gi`, or omit `--still` for the corresponding row):

```powershell
& "$env:TEMP/kiln-deferred-specular/release/kiln-sponza.exe" --rendering-driver vulkan --rendering-method kiln_deferred --log-file "$env:TEMP/kiln-bench.log" -- --benchmark --hardware --still --rays=2 --specular-rays=2 --size=1920x1080 --frames=480 "--output=$env:TEMP/kiln-bench"
```

The scene stores `frame_ms` beginning after frame 60; discard the first 120 stored intervals to use the same 180-frame warmup.
