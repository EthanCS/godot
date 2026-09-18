#!/usr/bin/env python3
"""Validate sun/sky reflection captures, demodulation and roughness response.

Temporal/settling statistics are descriptive; they are not reference-render error.
"""
import argparse
import json
from pathlib import Path

import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    checks, measurements, captures = {}, {}, {}
    for path in sorted(args.directory.iterdir()):
        if not (path / 'metadata.json').exists():
            continue
        m = json.loads((path / 'metadata.json').read_text())
        world = json.loads((path / 'world.json').read_text())
        name, w, h = path.name, m['width'], m['height']

        def rgba(key):
            return np.fromfile(path / (key + '.bin'), '<f2').reshape(h, w, 4).astype('f4')

        a, b, diffuse = rgba('specular'), rgba('fresnel'), rgba('diffuse')
        checks[name + '_finite_nonnegative'] = bool(all(np.isfinite(v).all() and (v >= 0).all() for v in (a, b, diffuse)))
        checks[name + '_scope'] = m['nrd_diffuse_rays'] == 0 and world['local_lights'] == 0 and world['emissive_triangles'] == 0
        checks[name + '_ray_budget'] = m['surfel_ray_budget'] == 196608 and m['surfel_rays'] <= 196608
        checks[name + '_ray_query'] = m.get('hardware_query_mismatches', 0) == 0
        normal = rgba('normal_roughness')
        lighting = json.loads((path / 'lighting.json').read_text())
        material = np.fromfile(path / 'material.bin', 'u1').reshape(h, w, 4).astype('f4') / 255
        albedo = np.fromfile(path / 'albedo_metallic.bin', 'u1').reshape(h, w, 4).astype('f4') / 255
        metallic = albedo[:, :, 3:4]
        f0 = .16 * material[:, :, 0:1] ** 2 * (1 - metallic) + albedo[:, :, :3] * metallic
        f90 = np.clip(50 * f0[:, :, 1:2], metallic, 1)
        reflected = f0 * a[:, :, :3] + f90 * b[:, :, :3]
        floor = ((normal[:, :, 1] > .98) & (np.indices((h, w))[0] > h * .55)
                 & (np.abs(normal[:, :, 3] - lighting['floor_roughness']) < .005))
        checks[name + '_floor_visible'] = floor.sum() > w * h * .02
        light = reflected @ np.array([.2126, .7152, .0722])
        surface = np.fromfile(path / 'surface.bin', '<u4').reshape(h, w, 2)[:, :, 0] > 0
        measurements[name] = {'floor_mean': float(light[floor].mean()), 'floor_p99': float(np.percentile(light[floor], 99)),
                              'roughness': float(np.median(normal[:, :, 3][floor])), 'nrd': m['nrd_active'],
                              'combined': m.get('nrd_combined_specular', False), 'checkerboard': m['nrd_checkerboard']}
        captures[name] = (light, floor, surface & ~floor)
        if name.startswith('r') and name[1:3].isdigit():
            checks[name + '_roughness'] = abs(measurements[name]['roughness'] - int(name[1:3]) / 100) < .005
        if m['nrd_active'] and m['specular_rays'] > 0:
            raw = rgba('specular_raw')
            packed = rgba('nrd_base')
            y, x = np.indices((h, w))
            selected = surface.copy()
            if m['nrd_checkerboard']:
                selected &= ((x ^ y ^ m['frame']) & 1) == 1
                packed = packed[y, x // 2]
            checks[name + '_real_hit_distance'] = bool(np.array_equal(packed[:, :, 3][selected], raw[:, :, 3][selected]))
            checks[name + '_valid_distances'] = bool((raw[:, :, 3][selected] >= 0).all() and (raw[:, :, 3][selected] <= 1000).all())
            if m.get('nrd_combined_specular'):
                basis = np.fromfile(path / 'nrd_specular_basis.bin', '<f2').reshape(h, w, 2).astype('f4')
                factor = f0 * basis[:, :, 0:1] + f90 * basis[:, :, 1:2]
                original = f0 * raw[:, :, :3] + f90 * rgba('fresnel_raw')[:, :, :3]
                roundtrip = packed[:, :, :3] * factor
                error = np.abs(roundtrip[selected] - original[selected]) / np.maximum(np.abs(original[selected]), .001)
                p99 = float(np.percentile(error, 99))
                measurements[name]['demodulation_roundtrip_p99_relative'] = p99
                checks[name + '_demodulation_roundtrip'] = p99 < .003
            checks[name + '_cache_input_unchanged'] = bool(np.array_equal(rgba('raw')[:, :, :3], rgba('nrd_diffuse_raw')[:, :, :3]))
    for r in (6, 18, 45, 85):
        key = f'r{r:02}'
        frames = [captures[f'{key}_noise_{i:02}'] for i in range(8)]
        mask = np.logical_and.reduce([f[1] for f in frames])
        values = np.stack([f[0][mask] for f in frames])
        settled, floor, nonfloor = captures[key + '_settled8']
        reference, _, reference_nonfloor = captures[key + '_settled96']
        nonfloor &= reference_nonfloor
        nonfloor_mask = np.logical_and.reduce([f[2] for f in frames])
        nonfloor_values = np.stack([f[0][nonfloor_mask] for f in frames])
        measurements[key + '_summary'] = {
            'temporal_std_over_mean': float(values.std(0).mean() / max(values.mean(), 1e-8)),
            'settled8_vs96_normalized_mae': float(np.abs(settled[floor] - reference[floor]).mean() / max(reference[floor].mean(), 1e-8)),
            'nonfloor_temporal_std_over_mean': float(nonfloor_values.std(0).mean() / max(nonfloor_values.mean(), 1e-8)),
            'nonfloor_settled8_vs96_normalized_mae': float(np.abs(settled[nonfloor] - reference[nonfloor]).mean() / max(reference[nonfloor].mean(), 1e-8)),
            'nonfloor_temporal_mean': float(nonfloor_values.mean()),
        }
        checks[key + '_reflection_present'] = measurements[key + '_lit']['floor_p99'] > .001
    checks['zero_f0'] = measurements['zero_f0']['floor_p99'] < 1e-7
    checks['specular_off'] = measurements['specular_off']['floor_p99'] == 0
    checks['gi_off'] = bool((captures['gi_off'][0] == 0).all())
    odd = json.loads((args.directory / 'odd_resize/metadata.json').read_text())
    checks['odd_dimensions'] = (odd['width'], odd['height']) == (961, 541)
    record = {'passed': bool(checks) and all(checks.values()), 'checks': {k: bool(v) for k, v in checks.items()}, 'measurements': measurements,
              'scope': 'Sun/sky only. Linear reflection metrics include per-pixel F0/F90, before engine multiscatter compensation and TAA. Temporal and settling are descriptive, not ground-truth error.'}
    (args.directory / 'specular_focus_checks.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({'passed': record['passed'], 'checks': len(checks), 'failed': [k for k, v in checks.items() if not v],
                      'roughness_metrics': {k: v for k, v in measurements.items() if k.endswith('_summary')}}, indent=2))
    raise SystemExit(0 if record['passed'] else 1)


if __name__ == '__main__':
    main()
