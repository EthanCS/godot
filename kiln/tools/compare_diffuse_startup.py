#!/usr/bin/env python3
"""Compare cold-cache linear diffuse at equal frame and primary-ray budgets.

The late frame is a convergence reference, not path-traced ground truth.
Readbacks stall rendering, so this reports frames rather than wall-clock speed.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from compare_diffuse_values import REGIONS, lowpass, read


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--plot', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    checks, measurements, images = {}, {}, {}
    for label, root in (('before', args.before), ('after', args.after)):
        _, reference, reference_masks = read(root, 'cold_0512', 'raw')
        measurements[label], images[label] = {}, {}
        for frame in (1, 4, 8, 16, 32, 64, 128, 240, 512):
            meta, light, masks = read(root, f'cold_{frame:04d}', 'raw')
            mask = masks['interior'] & reference_masks['interior']
            residual = light - lowpass(light, 16 * meta['width'] / 1920)
            mean = float(light[mask].mean())
            measurements[label][frame] = {
                'metadata': {k: meta[k] for k in ('frame', 'width', 'height', 'surfel_ray_budget', 'surfel_rays', 'surfel_alive', 'surfel_reconstruction')},
                'floor_mean': mean,
                'relative_residual': float(residual[mask].std() / max(mean, 1e-12)),
                'relative_rmse_to_512': float(np.sqrt(np.mean((light[mask] - reference[mask]) ** 2)) / reference[mask].mean()),
                'energy_ratio_to_512': float(mean / reference[mask].mean()),
            }
            checks[f'{label}_{frame}_ray_budget'] = meta['surfel_rays'] <= meta['surfel_ray_budget']
            checks[f'{label}_{frame}_capture_frame'] = meta['frame'] == frame - 1
            images[label][frame] = light
    for frame in measurements['before']:
        old, new = (measurements[label][frame]['metadata'] for label in ('before', 'after'))
        checks[f'{frame}_matching_setup'] = all(old[k] == new[k] for k in ('frame', 'width', 'height', 'surfel_ray_budget', 'surfel_reconstruction'))
    record = {'scope': __doc__, 'checks_passed': all(checks.values()), 'checks': checks, 'measurements': measurements}
    state_file = args.after / 'startup_state.json'
    if state_file.exists():
        revisions = json.loads(state_file.read_text())
        checks['no_spurious_material_revision'] = len({r['material_version'] for r in revisions}) == 1
        checks['cold_start_not_relighting'] = not any(r['moving'] for r in revisions)
    if (args.before / 'temporal_00').exists() and (args.after / 'temporal_00').exists():
        record['temporal'] = {}
        for label, root in (('before', args.before), ('after', args.after)):
            frames, masks = [], []
            for index in range(16):
                meta, light, regions = read(root, f'temporal_{index:02d}', 'raw')
                checks[f'{label}_temporal_{index}_frame'] = meta['frame'] == 512 + index
                frames.append(light)
                masks.append(regions['interior'])
            mask = np.logical_and.reduce(masks)
            values = np.stack(frames)[:, mask]
            record['temporal'][label] = float(values.std(0).mean() / values.mean())
    record['checks_passed'] = all(checks.values())
    (args.output / 'startup.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))
    if args.plot:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        frames = (8, 16, 32, 64, 128, 512)
        fig, axes = plt.subplots(2, len(frames), figsize=(16, 5), layout='constrained')
        for row, label in enumerate(('before', 'after')):
            for col, frame in enumerate(frames):
                light = images[label][frame]
                h, w = light.shape
                left, right, top, bottom = REGIONS['interior']
                axes[row, col].imshow(light[int(h * top):int(h * bottom), int(w * left):int(w * right)],
                                      cmap='gray', vmin=.035, vmax=.065, interpolation='nearest')
                axes[row, col].set_title(f'{label}, frame {frame}\nresidual {measurements[label][frame]["relative_residual"]:.2%}')
                axes[row, col].axis('off')
        fig.suptitle('Raw floor irradiance / pi; fixed linear scale 0.035–0.065; NRD off')
        fig.savefig(args.output / 'startup-floor.png', dpi=150)
        plt.close(fig)
    raise SystemExit(0 if record['checks_passed'] else 1)


if __name__ == '__main__':
    main()
