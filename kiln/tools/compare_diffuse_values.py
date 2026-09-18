#!/usr/bin/env python3
"""Compare linear GI readbacks, never composed screenshots or albedo detail.

Keep the historical floor mask and failed historical gate visible. The additional
interior patch excludes the plant silhouette whose zero values contaminate the
historical Gaussian neighborhood. Neither residual is path-traced error.
"""
import argparse
import json
from pathlib import Path

import numpy as np

LUMA = np.array([.2126, .7152, .0722])
REGIONS = {'historical': (.43, .59, .74, .94), 'interior': (.34, .52, .80, .96)}


def read(root, shot, signal):
    directory = root / shot
    metadata = json.loads((directory / 'metadata.json').read_text())
    h, w = metadata['height'], metadata['width']
    rgb = np.fromfile(directory / (signal + '.bin'), '<f2').reshape(h, w, 4).astype('f4')[:, :, :3]
    assert np.isfinite(rgb).all() and (rgb >= 0).all(), (directory, signal)
    normal = np.fromfile(directory / 'normal_roughness.bin', '<f2').reshape(h, w, 4)
    y, x = np.indices((h, w))
    masks = {}
    for name, (left, right, top, bottom) in REGIONS.items():
        mask = (x > w * left) & (x < w * right) & (y > h * top) & (y < h * bottom)
        mask &= (normal[:, :, 1] > .98) & (abs(normal[:, :, 3] - .18) < .01)
        assert mask.sum() > w * h * .015, (directory, name, 'insufficient planar floor')
        masks[name] = mask
    assert not metadata['nrd_active'] and not metadata['surfel_specular']
    return metadata, (rgb * LUMA).sum(2), masks


def lowpass(light, sigma):
    radius = int(3 * sigma)
    x = np.arange(-radius, radius + 1)
    kernel = np.exp(-x * x / (2 * sigma * sigma))
    kernel /= kernel.sum()
    for axis in (0, 1):
        light = np.apply_along_axis(lambda row: np.convolve(np.pad(row, radius, mode='edge'), kernel, 'valid'), axis, light)
    return light


def measure(root, shot, signal):
    meta, light, masks = read(root, shot, signal)
    result = {}
    for scale in (8, 24):
        residual = light - lowpass(light, scale * meta['width'] / 960)
        for name, mask in masks.items():
            result[f'{name}_sigma{scale}'] = {
                'mean': float(light[mask].mean()),
                'relative_residual': float(residual[mask].std() / max(light[mask].mean(), 1e-12)),
                'pixels': int(mask.sum()),
            }
    return meta, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--plot', action='store_true', help='Also write a fixed-scale plot (requires matplotlib).')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    record = {'scope': 'Linear irradiance/pi before material, AO, exposure, tonemapping and final TAA. Residual includes real gradients; no path-traced reference.', 'measurements': {}}
    for shot in ('still', 'converged', 'returned', 'sunset'):
        for signal in ('raw', 'diffuse'):
            old_meta, before = measure(args.before, shot, signal)
            new_meta, after = measure(args.after, shot, signal)
            for key in ('width', 'height', 'frame', 'surfel_ray_budget', 'surfel_reconstruction'):
                assert old_meta[key] == new_meta[key], (shot, key)
            values = {}
            for key in before:
                values[key] = {'before': before[key], 'after': after[key],
                               'residual_reduction': 1 - after[key]['relative_residual'] / before[key]['relative_residual'],
                               'energy_ratio': after[key]['mean'] / before[key]['mean']}
            record['measurements'][shot + '_' + signal] = values
    # Retain the old 35% requirement for the *filtered* historical patch. This
    # must not be silently replaced by a successful raw/interior measurement.
    record['historical_filtered_35pct_gate'] = record['measurements']['still_diffuse']['historical_sigma8']['residual_reduction'] > .35
    if (args.before / 'temporal_00').exists() and (args.after / 'temporal_00').exists():
        record['temporal'] = {}
        for signal in ('raw', 'diffuse'):
            values = []
            for root in (args.before, args.after):
                frames, masks = [], []
                for i in range(16):
                    _, light, mask = read(root, f'temporal_{i:02d}', signal)
                    frames.append(light)
                    masks.append(mask['interior'])
                data = np.stack(frames)
                mask = np.logical_and.reduce(masks)
                values.append(float(data[:, mask].std(0).mean() / data[:, mask].mean()))
            record['temporal'][signal] = {'before_relative_std': values[0], 'after_relative_std': values[1], 'reduction': 1 - values[1] / values[0]}
    (args.output / 'comparison.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))
    if args.plot:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        from matplotlib.patches import Rectangle
        for shot in ('still', 'converged', 'returned'):
            fig, axes = plt.subplots(3, 2, figsize=(12, 10), layout='constrained')
            for col, (label, root) in enumerate((('Before', args.before), ('After', args.after))):
                meta, light, _ = read(root, shot, 'raw')
                h, w = light.shape
                left, right, top, bottom = REGIONS['interior']
                x0, x1, y0, y1 = int(w * left), int(w * right), int(h * top), int(h * bottom)
                axes[0, col].imshow(light, cmap='gray', vmin=0, vmax=.10)
                axes[0, col].add_patch(Rectangle((x0, y0), x1 - x0, y1 - y0, fill=False, edgecolor='#e0a344'))
                axes[0, col].set_title(f'{label}: raw irradiance/pi, frame {meta["frame"]}')
                axes[1, col].imshow(light[y0:y1, x0:x1], cmap='gray', vmin=.035, vmax=.065, interpolation='nearest')
                axes[1, col].set_title('Floor values: identical linear scale 0.035-0.065')
                row = (y0 + y1) // 2
                axes[2, col].plot(np.arange(x0, x1), light[row, x0:x1], linewidth=1.3)
                axes[2, col].set_ylim(.035, .065)
                axes[2, col].set_title(f'Unsmoothed numeric profile at y={row}')
                axes[2, col].set_xlabel('Pixel x')
                axes[2, col].set_ylabel('Irradiance / pi')
            fig.savefig(args.output / (shot + '-values.png'), dpi=150)
            plt.close(fig)


if __name__ == '__main__':
    main()
