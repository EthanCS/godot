#!/usr/bin/env python3
"""Validate diffuse-only GPU captures and optional irradiance-sharing ablation.

Spatial residual is descriptive, not error against a path-traced reference.
No reference threshold is inferred from the measured candidate.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
parser.add_argument('--without-sharing', type=Path)
args = parser.parse_args()
checks, measurements = {}, {}


def spatial(root, shot='still', signal='raw'):
    path = root / shot
    meta = json.loads((path / 'metadata.json').read_text())
    w, h = meta['width'], meta['height']
    rgb = np.fromfile(path / (signal + '.bin'), '<f2').reshape(h, w, 4).astype('f4')[:, :, :3]
    normal = np.fromfile(path / 'normal_roughness.bin', '<f2').reshape(h, w, 4)
    y, x = np.indices((h, w))
    mask = (x > w * .43) & (x < w * .59) & (y > h * .74) & (y < h * .94)
    mask &= (normal[:, :, 1] > .98) & (abs(normal[:, :, 3] - .18) < .01)
    assert mask.sum() > w * h * .015
    light = rgb @ np.array([.2126, .7152, .0722])
    sigma = 8 * w / 960
    radius = int(sigma * 3)
    x = np.arange(-radius, radius + 1)
    kernel = np.exp(-x * x / (2 * sigma * sigma))
    kernel /= kernel.sum()
    low = light.copy()
    for axis in (0, 1):
        low = np.apply_along_axis(lambda v: np.convolve(np.pad(v, radius, mode='edge'), kernel, mode='valid'), axis, low)
    return {'mean': float(light[mask].mean()),
            'relative_residual': float((light - low)[mask].std() / light[mask].mean()),
            'width': w, 'height': h}


for path in sorted(args.directory.iterdir()):
    if not (path / 'metadata.json').exists():
        continue
    m = json.loads((path / 'metadata.json').read_text())
    world = json.loads((path / 'world.json').read_text())
    name, w, h = path.name, m['width'], m['height']
    checks[name + '_diffuse_only'] = not m['nrd_active'] and not m['surfel_specular'] and m['nrd_diffuse_rays'] == 0
    checks[name + '_sun_sky_only'] = world['local_lights'] == 0 and world['emissive_triangles'] == 0
    surface = np.fromfile(path / 'surface.bin', '<u4').reshape(h, w, 2)[:, :, 0] > 0
    confidence = np.fromfile(path / 'confidence.bin', '<f2').reshape(h, w)
    coverage = float((confidence[surface] > 0).mean())
    checks[name + '_coverage'] = coverage > .985
    rays = np.fromfile(path / 'ray_results.bin', '<f4').reshape(-1, 4)
    active = rays[:, 3] > 0
    checks[name + '_actual_ray_count'] = int(rays[:, 3].sum()) == m['surfel_rays']
    checks[name + '_ray_limit'] = bool((rays[:, 3] >= 0).all() and (rays[:, 3] <= 32).all())
    checks[name + '_budget'] = m['surfel_ray_budget'] == 0 or m['surfel_rays'] <= m['surfel_ray_budget']
    schedule = np.fromfile(path / 'ray_schedule.bin', '<u4')
    requests = schedule[4:].reshape(-1, 2)
    requests = requests[requests[:, 0] > 0]
    requests = requests[np.argsort(requests[:, 1])]
    prefix = np.cumsum(requests[:, 0], dtype=np.uint64) - requests[:, 0]
    checks[name + '_schedule_prefix'] = bool(np.array_equal(prefix, requests[:, 1]) and requests[:, 0].sum() == schedule[0])
    shared = np.fromfile(path / 'shared_samples.bin', '<f4').reshape(-1, 4)
    history = np.fromfile(path / 'sample_history.bin', '<f4').reshape(-1, 4)
    checks[name + '_finite_batches'] = bool(np.isfinite(shared[active]).all() and np.isfinite(history[active]).all())
    checks[name + '_nonnegative_batches'] = bool((shared[active, :3] >= 0).all() and (history[active, :3] >= 0).all())
    if not m['surfel_reconstruction']:
        raw = np.fromfile(path / 'raw.bin', '<f2').reshape(h, w, 4)
        diffuse = np.fromfile(path / 'diffuse.bin', '<f2').reshape(h, w, 4)
        checks[name + '_raw_bypass'] = bool(np.array_equal(raw[:, :, :3], diffuse[:, :, :3]))
    measurements[name] = {'coverage': coverage, 'primary_rays': m['surfel_rays'],
                          'nominal_rays': int(schedule[1]), 'surfel_alive': m['surfel_alive']}

odd = json.loads((args.directory / 'odd_resize/metadata.json').read_text())
checks['odd_dimensions'] = (odd['width'], odd['height']) == (961, 541)
measurements['still_spatial'] = spatial(args.directory)
measurements['converged_spatial'] = spatial(args.directory, 'converged')
checks['cache_converges'] = measurements['converged_spatial']['relative_residual'] < measurements['still_spatial']['relative_residual']
frames = np.stack([np.asarray(Image.open(args.directory / f'still_{i:02}.png'), dtype='f4') / 255 for i in range(16)])
measurements['display_temporal_std'] = float(frames.std(0).mean())
checks['stationary_display_stability'] = measurements['display_temporal_std'] < .005
if args.without_sharing:
    before, after = spatial(args.without_sharing), spatial(args.directory)
    assert (before['width'], before['height']) == (after['width'], after['height'])
    reduction = 1 - after['relative_residual'] / before['relative_residual']
    ratio = after['mean'] / before['mean']
    measurements['sharing_ablation'] = {'without': before, 'with': after, 'residual_reduction': reduction, 'energy_ratio': ratio}
    checks['sharing_reduces_raw_blotches'] = reduction > .35
    checks['sharing_preserves_energy'] = .9 < ratio < 1.1
record = {'passed': bool(checks) and all(checks.values()), 'checks': checks, 'measurements': measurements,
          'scope': 'Real GPU sun/sky diffuse only; primary budget excludes shadow/continuation rays. Spatial residual includes real gradients; display stability includes TAA.'}
(args.directory / 'diffuse_checks.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps({'passed': record['passed'], 'failed': [k for k, v in checks.items() if not v], 'measurements': measurements}, indent=2))
raise SystemExit(0 if record['passed'] else 1)
