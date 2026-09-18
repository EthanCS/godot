#!/usr/bin/env python3
"""Measure dynamic raw-cache noise, response and surfel survival across resize.

All comparisons use linear irradiance/pi, before material, AO or screen filters.
Spatial high-pass residual includes real gradients. Its inter-frame change is
not path-traced error; TOD shadow motion can contribute. Readbacks are not timing.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from compare_diffuse_values import REGIONS, lowpass, read


def inspect(root):
    shots, temporal, checks = {}, {}, {}
    for directory in sorted(root.glob('*_*')):
        if not (directory / 'metadata.json').exists():
            continue
        meta, light, masks = read(root, directory.name, 'raw')
        mask = masks['interior']
        mean = float(light[mask].mean())
        residual = light - lowpass(light, 16 * meta['width'] / 1920)
        shots[directory.name] = {
            'frame': meta['frame'], 'size': [meta['width'], meta['height']],
            'mean': mean, 'relative_residual': float(residual[mask].std() / max(mean, 1e-12)),
            'rays': meta['surfel_rays'], 'alive': meta['surfel_alive'],
            'budget': meta['surfel_ray_budget'], 'sharing': meta['irradiance_sharing'],
            'reconstruction': meta['surfel_reconstruction'],
        }
        checks[directory.name + '_budget'] = meta['surfel_rays'] <= meta['surfel_ray_budget']
        checks[directory.name + '_queries'] = meta['hardware_query_mismatches'] == 0
        if directory.name in ('small_0001', 'large_0001') and 'screen_history_valid' in meta:
            checks[directory.name + '_screen_history_restarts'] = not meta['screen_history_valid']
    for series in ('static', 'tod'):
        frames, masks = [], []
        for directory in sorted(root.glob(series + '_*')):
            meta, light, regions = read(root, directory.name, 'raw')
            frames.append(light)
            masks.append(regions['interior'])
        assert len(frames) == 16, series
        mask = np.logical_and.reduce(masks)
        data = np.stack(frames)
        # Remove smooth genuine illumination changes before measuring flicker.
        changes = np.diff(data, axis=0)
        hp = np.stack([v - lowpass(v, 16 * meta['width'] / 1920) for v in changes])
        temporal[series] = float(hp[:, mask].std() / data[:, mask].mean())
    persistence = {}
    for old_name, new_name in (('step_0256', 'small_0001'), ('small_0032', 'large_0001')):
        old = np.fromfile(root / old_name / 'surfels.bin', '<f4').reshape(-1, 32)
        new_data = np.fromfile(root / new_name / 'surfels.bin', '<f4').reshape(-1, 32)
        new = np.zeros_like(old)
        common = min(len(old), len(new_data))
        new[:common] = new_data[:common]
        old_u, new_u = old.view('<u4'), new.view('<u4')
        alive = old_u[:, 22] > 0
        same = ((new_u[:, 22] > 0) & (new_u[:, 20] == old_u[:, 20]) &
                np.all(old[:, 24:27] == new[:, 24:27], axis=1))
        retained = alive & same & (new_u[:, 23] >= old_u[:, 23]) & (new[:, 11] >= old[:, 11])
        persistence[new_name] = float(retained.sum() / max(alive.sum(), 1))
    return {'shots': shots, 'temporal': temporal, 'retained_surfels': persistence, 'checks': checks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--plot', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    before, after = inspect(args.before), inspect(args.after)
    a, b = after['shots'], before['shots']
    assert len(a) == len(b) == 61 and a.keys() == b.keys(), 'Incomplete capture sequence'
    checks = {
        'tod_flicker_reduced_60pct': after['temporal']['tod'] < before['temporal']['tod'] * .4,
        'step32_within_5pct_of_late_before': abs(a['step_0032']['mean'] / b['step_0256']['mean'] - 1) < .05,
        'off32_below_1pct': a['off_0032']['mean'] < a['large_0128']['mean'] * .01,
        'off128_below_0_1pct': a['off_0128']['mean'] < a['large_0128']['mean'] * .001,
        'restored128_within_5pct': abs(a['restore_0128']['mean'] / a['large_0128']['mean'] - 1) < .05,
        'resize_down_retains_99pct': after['retained_surfels']['small_0001'] > .99,
        'resize_up_retains_99pct': after['retained_surfels']['large_0001'] > .99,
        'resize_down_frame_continuity': a['small_0001']['frame'] == a['step_0256']['frame'] + 1,
        'resize_up_frame_continuity': a['large_0001']['frame'] == a['small_0032']['frame'] + 1,
    }
    for name in a:
        checks[name + '_matching_setup'] = all(a[name][key] == b[name][key] for key in ('size', 'budget', 'sharing', 'reconstruction'))
    for name in ('small_0001', 'large_0001', 'large_0002'):
        checks[name + '_residual_below_3pct'] = a[name]['relative_residual'] < .03
    record = {'scope': __doc__, 'before': before, 'after': after, 'checks': checks,
              'passed': all(checks.values()) and all(before['checks'].values()) and all(after['checks'].values())}
    (args.output / 'comparison.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({'passed': record['passed'], 'checks': checks, 'temporal': [before['temporal'], after['temporal']],
                      'retained': after['retained_surfels']}, indent=2))
    if args.plot:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        names = ('tod_0180', 'step_0032', 'small_0001', 'large_0001')
        fig, axes = plt.subplots(2, len(names), figsize=(14, 5), layout='constrained')
        for row, (root, values, title) in enumerate(((args.before, b, 'Before'), (args.after, a, 'After'))):
            for col, name in enumerate(names):
                _, light, _ = read(root, name, 'raw')
                h, w = light.shape
                left, right, top, bottom = REGIONS['interior']
                # One fixed scale per scenario, identical before/after.
                center = .028 if name.startswith('tod') else .014
                axes[row, col].imshow(light[int(h * top):int(h * bottom), int(w * left):int(w * right)],
                                      cmap='gray', vmin=center * .65, vmax=center * 1.35, interpolation='nearest')
                axes[row, col].set_title(f'{title} {name}\nresidual {values[name]["relative_residual"]:.2%}')
                axes[row, col].axis('off')
        fig.suptitle('Raw floor irradiance/pi; matching scale per column; no NRD or TAA')
        fig.savefig(args.output / 'floor.png', dpi=140)
        plt.close(fig)
    raise SystemExit(0 if record['passed'] else 1)


if __name__ == '__main__':
    main()
