#!/usr/bin/env python3
"""Verify diagnostic pixels against actual GPU cache entries, not screenshots alone."""
import argparse
import json
from pathlib import Path

import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument("directory", type=Path)
args = parser.parse_args()
root = args.directory
checks = {}
measurements = {}
luma = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
for mode in range(15, 28):
    folder = root / f"surfel_{mode:02}"
    metadata = json.loads((folder / "metadata.json").read_text())
    h, w = metadata["height"], metadata["width"]
    data = np.fromfile(folder / "debug_metrics.bin", "<f4").reshape(h, w, 4)
    color = np.fromfile(folder / "debug.bin", "<f2").reshape(h, w, 4).astype("f4")
    cache = np.fromfile(folder / "surfels.bin", "<f4").reshape(-1, 32)
    rays = np.fromfile(folder / "ray_results.bin", "<f4").reshape(-1, 4)
    confidence = np.fromfile(folder / "confidence.bin", "<f2").reshape(h, w).astype("f4") / 256
    selected = data[..., 0] > 0
    ids = data[..., 0][selected].astype(int) - 1
    surfels = cache[ids]
    value = data[..., 3][selected]
    prefix = f"{mode}_"
    checks[prefix + "mode"] = metadata["surfel_debug_mode"] == mode
    checks[prefix + "finite"] = np.isfinite(data).all() and np.isfinite(color).all()
    checks[prefix + "real_cache_entries"] = len(np.unique(ids)) > 256 and (surfels.view("<u4")[:, 22] != 0).all()
    checks[prefix + "coverage_matches_gather"] = np.allclose(np.minimum(data[..., 1], 1), confidence, atol=0.0006)
    checks[prefix + "visible_output"] = selected.mean() > 0.03 and color[..., :3].std() > 0.01
    measurements[str(mode)] = {"selected_pixel_fraction": float(selected.mean()), "distinct_surfels": int(len(np.unique(ids)))}
    expected = None
    if mode == 16:
        expected = surfels[:, 8:11] @ luma
    elif mode == 18:
        expected = surfels[:, 3]
    elif mode == 19:
        expected = surfels[:, 7]
    elif mode == 20:
        expected = surfels[:, 11]
    elif mode == 21:
        expected = rays[ids, 3]
        checks["updates_show_traced_and_reused"] = (value > 0).mean() > 0.05 and (value == 0).mean() > 0.05
    elif mode == 22:
        expected = data[..., 1][selected]
    elif mode == 23:
        expected = data[..., 2][selected]
    elif mode == 24:
        expected = np.sqrt(np.maximum(surfels[:, 16:19] @ luma, 0)) / np.maximum(surfels[:, 12:15] @ luma, 0.01)
    elif mode == 25:
        raw = np.fromfile(folder / "raw.bin", "<f2").reshape(h, w, 4).astype("f4")
        expected = raw[selected, :3] @ luma
    elif mode == 26:
        expected = np.where(surfels[:, 3] <= 0.25, 0, np.where(surfels[:, 3] <= 0.5, 1, 2))
        checks["grid_shows_multiple_levels"] = len(np.unique(value)) >= 2
    elif mode == 27:
        world = json.loads((folder / "world.json").read_text())
        expected = (surfels.view("<u4")[:, 20] - 1 >= world["static_triangles"]).astype(float)
        checks["anchors_show_static_and_dynamic"] = (value == 0).any() and (value == 1).any()
    if expected is not None:
        checks[prefix + "matches_gpu_cache_value"] = np.allclose(value, expected, atol=0.0001, rtol=0.0001)

cold = np.fromfile(root / "cold_age/debug_metrics.bin", "<f4").reshape(-1, 4)
checks["reset_shows_new_surfels"] = cold[cold[:, 0] > 0, 3].max() <= 8
off = np.fromfile(root / "debug_gi_off/debug.bin", "<f2").reshape(-1, 4)
checks["disabled_gi_has_no_stale_debug_output"] = (off[:, :3] == 0).all()
odd = json.loads((root / "debug_odd_resize/metadata.json").read_text())
checks["odd_resize"] = odd["width"] == 961 and odd["height"] == 541
lit = json.loads((root / "debug_return_lit/metadata.json").read_text())
checks["return_to_lit_disables_debug_pass"] = lit["surfel_debug_mode"] == 0
checks = {name: bool(value) for name, value in checks.items()}
record = {"passed": all(checks.values()), "checks": checks, "measurements": measurements}
(root / "debug_checks.json").write_text(json.dumps(record, indent=2) + "\n")
print(json.dumps({"passed": record["passed"], "checks": len(checks), "failed": [k for k, v in checks.items() if not v]}, indent=2))
raise SystemExit(0 if record["passed"] else 1)
