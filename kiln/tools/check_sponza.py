#!/usr/bin/env python3
"""Numerical GI acceptance on real Sponza captures (not a visual/performance pass).

Predeclared limits: finite buffers; CPU/GPU BVH bounds within 0.003 world units;
day/local mean > 0.001 above dark; emission channel dominance > 1.5;
32-frame off residual < 0.005; stationary display standard deviation < 0.005.
"""
from pathlib import Path
import json
import sys
import numpy as np
from PIL import Image

root = Path(sys.argv[1])
checks, measurements, captures = {}, {}, {}
for directory in sorted(root.iterdir()):
    if not (directory / 'metadata.json').exists():
        continue
    metadata = json.loads((directory / 'metadata.json').read_text())
    h, w = metadata['half_height'], metadata['half_width']
    pos = np.fromfile(directory / 'position.bin', '<f4').reshape(h, w, 4)
    gi = np.fromfile(directory / 'decoded.bin', '<f2').reshape(h, w, 4).astype('f4')
    sh = np.fromfile(directory / 'sh.bin', '<f4')
    valid = pos[:, :, 3] > 0.5
    checks[directory.name + '_finite'] = bool(np.isfinite(gi).all() and np.isfinite(sh).all() and np.isfinite(pos).all())
    checks[directory.name + '_compute_bvh'] = metadata['compute_bvh_maximum_error'] < 0.003
    measurements[directory.name] = gi[valid, :3].mean(0).tolist()
    captures[directory.name] = (pos, gi, valid)

def mean(name):
    return np.array(measurements[name])

checks['bvh_brute_force'] = json.loads((root / 'bvh.json').read_text())['passed']
checks['dark_no_ambient_injection'] = np.linalg.norm(mean('00_dark')) < 0.0001
checks['sun_sky_transport'] = mean('01_day').mean() > mean('00_dark').mean() + 0.001
checks['tod_changes_transport'] = np.linalg.norm(mean('01_day') - mean('02_sunset')) > 0.001
checks['multiple_local_lights'] = mean('03_local').mean() > mean('00_dark').mean() + 0.001
red, blue = mean('04_emission_red'), mean('05_emission_blue')
checks['emission_red'] = red[0] > 0.001 and red[0] > red[2] * 1.5
checks['emission_blue_material_update'] = blue[2] > 0.001 and blue[2] > blue[0] * 1.5
checks['off_32_residual'] = np.linalg.norm(mean('06_off_32')) < 0.005
worlds = [json.loads((root / name / 'world.json').read_text()) for name in ['04_emission_red', '05_emission_blue']]
checks['material_updates_without_bvh_rebuild'] = worlds[1]['material_version'] > worlds[0]['material_version'] and worlds[1]['geometry_version'] == worlds[0]['geometry_version'] and worlds[1]['dynamic_version'] == worlds[0]['dynamic_version']
checks['simplification_reduces_geometry'] = worlds[0]['proxy_triangles_unique'] < worlds[0]['source_triangles_unique'] * 0.7
images = np.stack([np.asarray(Image.open(root / f'noise_{i:02}' / 'color.png').convert('RGB'), dtype=float) / 255.0 for i in range(8)])
variation = float(images.std(0).mean())
measurements['stationary_display_rgb_std'] = variation
checks['stationary_temporal_noise'] = variation < 0.005
if (root / 'motion_settled_32').exists():
    def display(name):
        return np.asarray(Image.open(root / name / 'color.png').convert('RGB'), dtype=float) / 255.0
    settled_error = float(np.abs(display('motion_settled_32') - display('07_settled')).mean())
    measurements['motion_settled32_display_mae'] = settled_error
    checks['motion_settled32_error'] = settled_error < 0.035
checks = {name: bool(value) for name, value in checks.items()}
result = {'passed': all(checks.values()), 'checks': checks, 'measurements': measurements}
(root / 'checks.json').write_text(json.dumps(result, indent=2))
print(json.dumps(result, indent=2))
sys.exit(0 if result['passed'] else 1)
