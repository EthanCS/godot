#!/usr/bin/env python3
"""Check real-GPU cache -> RELAX captures; compare both filters on identical inputs.

The spatial high-pass residual is descriptive, not path-traced reference error.
Validation mode runs the fallback in parallel and must never be used for timing.
"""
import argparse
import json
from pathlib import Path

import numpy as np


def rgba(path, name, w, h):
    return np.fromfile(path / (name + '.bin'), '<f2').reshape(h, w, 4).astype('f4')


def floor_mask(path, w, h):
    normal = rgba(path, 'normal_roughness', w, h)
    y, x = np.indices((h, w))
    return ((x > w * .43) & (x < w * .59) & (y > h * .74) & (y < h * .94)
            & (normal[:, :, 1] > .98) & (abs(normal[:, :, 3] - .18) < .01))


def spatial(rgb, mask, w):
    light = rgb @ np.array([.2126, .7152, .0722])
    sigma = 8 * w / 960
    radius = int(sigma * 3)
    offset = np.arange(-radius, radius + 1)
    kernel = np.exp(-offset ** 2 / (2 * sigma * sigma))
    kernel /= kernel.sum()
    low = light.copy()
    for axis in (0, 1):
        low = np.apply_along_axis(lambda v: np.convolve(np.pad(v, radius, mode='edge'), kernel, mode='valid'), axis, low)
    return {'mean': float(light[mask].mean()),
            'relative_residual': float((light - low)[mask].std() / max(light[mask].mean(), 1e-8))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    checks, measurements, temporal = {}, {}, {}
    masks = []
    for path in sorted(args.directory.iterdir()):
        if not (path / 'metadata.json').exists():
            continue
        m = json.loads((path / 'metadata.json').read_text())
        world = json.loads((path / 'world.json').read_text())
        name, w, h = path.name, m['width'], m['height']
        checks[name + '_scope'] = (not m['surfel_specular'] and m['specular_rays'] == 0
                                   and m['nrd_diffuse_rays'] == 0 and world['local_lights'] == 0
                                   and world['emissive_triangles'] == 0)
        checks[name + '_ray_budget'] = m['surfel_ray_budget'] == 196608 and m['surfel_rays'] <= 196608
        checks[name + '_sharing'] = m['irradiance_sharing'] and m['surfel_reconstruction']
        surface = np.fromfile(path / 'surface.bin', '<u4').reshape(h, w, 2)[:, :, 0] > 0
        confidence = np.fromfile(path / 'confidence.bin', '<f2').reshape(h, w)
        coverage = float((confidence[surface] > 0).mean())
        checks[name + '_coverage'] = coverage > .985
        signals = {s: rgba(path, s, w, h)[:, :, :3] for s in ('raw', 'diffuse')}
        for s, a in signals.items():
            checks[name + '_' + s + '_finite_nonnegative'] = bool(np.isfinite(a).all() and (a >= 0).all())
        measurements[name] = {'coverage': coverage, 'primary_rays': m['surfel_rays']}
        if name == 'nrd_off':
            checks[name + '_disabled'] = not m['nrd_active']
            continue
        checks[name + '_active_full_rate'] = m['nrd_active'] and not m['nrd_checkerboard'] and m['nrd_signals'] == 'cache_diffuse'
        packed = rgba(path, 'nrd_diffuse_raw', w, h)
        checks[name + '_unchanged_cache_input'] = bool(np.array_equal(packed[surface, :3], signals['raw'][surface]))
        checks[name + '_unused_hit_distance'] = bool((packed[:, :, 3] == 0).all())
        depth = np.fromfile(path / 'nrd_depth.bin', '<f4').reshape(h, w)
        motion = rgba(path, 'nrd_motion', w, h)
        checks[name + '_guides'] = bool(np.isfinite(depth).all() and (depth > 0).all()
                                        and (depth[surface] < 10000).all() and np.isfinite(motion).all())
        checks[name + '_sky_cleared'] = bool((signals['diffuse'][depth >= 10000] == 0).all())
        if name.startswith('motion_'):
            measurements[name]['motion_p95_uv'] = float(np.percentile(np.linalg.norm(motion[surface, :2], axis=1), 95))
        if m['nrd_same_input_reference']:
            signals['diffuse_reference'] = rgba(path, 'diffuse_reference', w, h)[:, :, :3]
            if name.startswith('temporal_'):
                for s, a in signals.items():
                    temporal.setdefault(s, []).append(a)
                masks.append(floor_mask(path, w, h))
            if name in ('still', 'converged', 'returned', 'sunset', 'resize_restored'):
                mask = floor_mask(path, w, h)
                assert mask.sum() > w * h * .015
                stats = {s: spatial(a, mask, w) for s, a in signals.items()}
                reference, candidate = stats['diffuse_reference'], stats['diffuse']
                stats['residual_reduction'] = 1 - candidate['relative_residual'] / reference['relative_residual']
                stats['energy_ratio'] = candidate['mean'] / reference['mean']
                measurements[name]['floor'] = stats
                checks[name + '_energy'] = .9 < stats['energy_ratio'] < 1.1
    if temporal:
        mask = np.logical_and.reduce(masks)
        stats = {}
        for s, frames in temporal.items():
            frames = np.stack([a[mask] for a in frames])
            light = frames @ np.array([.2126, .7152, .0722])
            stats[s] = {'relative_temporal_std': float(light.std(0).mean() / light.mean()), 'samples': len(frames)}
        stats['reduction'] = 1 - stats['diffuse']['relative_temporal_std'] / stats['diffuse_reference']['relative_temporal_std']
        measurements['floor_temporal'] = stats
        checks['temporal_samples'] = len(masks) == 16
    odd = json.loads((args.directory / 'odd_resize/metadata.json').read_text())
    checks['odd_dimensions'] = (odd['width'], odd['height']) == (961, 541)
    record = {'passed': bool(checks) and all(checks.values()), 'checks': checks, 'measurements': measurements,
              'scope': 'Sun/sky diffuse only. Raw linear signals before materials, exposure and TAA. Same-input reference is the original two-pass spatial + temporal filter. Residual is not reference error; no persistent-blotch removal gate is asserted.'}
    (args.directory / 'nrd_cache_checks.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({'passed': record['passed'], 'checks': len(checks), 'failed': [k for k, v in checks.items() if not v],
                      'measurements': measurements}, indent=2))
    raise SystemExit(0 if record['passed'] else 1)


if __name__ == '__main__':
    main()
