#!/usr/bin/env python3
"""Compare identical real-GPU still captures, including spatial GI blotches.

The spatial metric is a descriptive high-pass residual, not reference error.
Texture-free linear diffuse readbacks avoid mistaking albedo detail for noise.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument('before', type=Path)
parser.add_argument('after', type=Path)
args = parser.parse_args()


def smooth(a, sigma=8):
    radius = int(sigma * 3)
    x = np.arange(-radius, radius + 1)
    kernel = np.exp(-x * x / (2 * sigma * sigma))
    kernel /= kernel.sum()
    for axis in (0, 1):
        a = np.apply_along_axis(lambda v: np.convolve(np.pad(v, radius, mode='edge'), kernel, mode='valid'), axis, a)
    return a


def read(root):
    capture = root / 'still'
    meta = json.loads((capture / 'metadata.json').read_text())
    h, w = meta['height'], meta['width']
    diffuse = np.fromfile(capture / 'diffuse.bin', '<f2').reshape(h, w, 4).astype('f4')[:, :, :3]
    normals = np.fromfile(capture / 'normal_roughness.bin', '<f2').reshape(h, w, 4)
    y, x = np.indices((h, w))
    # Fixed central floor patch, away from columns, silhouette and contact edges.
    floor = (x > w * .43) & (x < w * .59) & (y > h * .74) & (y < h * .94)
    floor &= (normals[:, :, 1] > .98) & (abs(normals[:, :, 3] - .18) < .01)
    assert floor.sum() > w * h * .015, 'Insufficient matching planar floor'
    light = diffuse @ np.array([.2126, .7152, .0722])
    residual = light - smooth(light)
    assert np.isfinite(diffuse).all() and (diffuse >= 0).all()
    values = {
        'floor_pixels': int(floor.sum()),
        'floor_mean_irradiance': float(light[floor].mean()),
        'floor_spatial_residual_std': float(residual[floor].std()),
        'floor_relative_spatial_residual': float(residual[floor].std() / light[floor].mean()),
    }
    for mode in ['still', 'tod']:
        frames = np.stack([np.asarray(Image.open(root / f'{mode}_{i:02d}.png'), dtype='f4') / 255 for i in range(16)])
        # TOD images contain real illumination change; this is not pure noise.
        values[mode + '_display_temporal_std'] = float(frames.std(0).mean())
    Image.fromarray((np.clip(1 - np.exp(-diffuse * 8), 0, 1) * 255).astype('uint8')).save(root / 'still_diffuse.png')
    return meta, values


before_meta, before = read(args.before)
after_meta, after = read(args.after)
assert (before_meta['width'], before_meta['height']) == (after_meta['width'], after_meta['height'])
reduction = 1 - after['floor_relative_spatial_residual'] / before['floor_relative_spatial_residual']
mean_ratio = after['floor_mean_irradiance'] / before['floor_mean_irradiance']
checks = {
    'static_spatial_blotches_reduced': reduction > .35,
    'floor_energy_retained': .75 < mean_ratio < 1.25,
    'stationary_temporal_noise': after['still_display_temporal_std'] < .005,
}
record = {'passed': all(checks.values()), 'checks': checks, 'before': before, 'after': after,
          'spatial_residual_reduction': reduction, 'floor_mean_ratio': mean_ratio,
          'scope': f"Same {before_meta['width']}x{before_meta['height']} stationary Sponza camera; descriptive spatial residual, not reference error."}
(args.after / 'noise_checks.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record, indent=2))
raise SystemExit(0 if record['passed'] else 1)
