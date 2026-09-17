#!/usr/bin/env python3
"""Compare handwritten and translated reflection passes on identical GPU inputs."""
import argparse
import json
from pathlib import Path

import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
args = parser.parse_args()
results = {}
for path in sorted(args.directory.rglob('metadata.json')):
    metadata = json.loads(path.read_text())
    if metadata.get('specular_reference') != 'same_frame_translated_glsl':
        continue
    assert metadata['specular_implementation'] == 'handwritten_msl', path
    h, w = metadata['height'], metadata['width']
    def read(name):
        return np.fromfile(path.parent / (name + '.bin'), '<f2').astype('f4').reshape(h, w, 4)
    base, reference = read('specular_raw'), read('specular_reference')
    fresnel, reference_fresnel = read('fresnel_raw'), read('fresnel_reference')
    assert all(np.isfinite(a).all() for a in [base, reference, fresnel, reference_fresnel]), path
    # The negative distance sentinel marks a deliberately omitted rough sample.
    marker_mismatches = int(np.count_nonzero((base[..., 3] < 0) != (reference[..., 3] < 0)))
    assert marker_mismatches == 0, (path, marker_mismatches)
    entry = {'size': [w, h], 'rays': metadata['specular_rays'], 'marker_mismatches': marker_mismatches}
    for channel, a, b in [('base', base, reference), ('fresnel', fresnel, reference_fresnel)]:
        rgb, ref = a[..., :3], b[..., :3]
        error = np.abs(rgb - ref)
        mean_error = float(error.mean())
        reference_mean = float(np.abs(ref).mean())
        assert mean_error <= 0.0001 + 0.001 * reference_mean, (path, channel, mean_error, reference_mean)
        relative = error / np.maximum(0.01, np.maximum(np.abs(rgb), np.abs(ref)))
        q999 = float(np.quantile(relative, 0.999))
        assert q999 < 0.02, (path, channel, q999)
        entry[channel] = {'mean_absolute_error': mean_error, 'reference_mean': reference_mean,
                          'maximum_absolute_error': float(error.max()), 'p99_9_relative_error': q999}
    distance_error = np.abs(base[..., 3] - reference[..., 3])
    entry['distance_p99_9_error'] = float(np.quantile(distance_error, 0.999))
    assert entry['distance_p99_9_error'] < 0.05, (path, entry)
    results[str(path.parent.relative_to(args.directory))] = entry
assert results, 'No same-frame reflection reference captures found'
record = {'passed': True, 'captures': results}
(args.directory / 'metal_specular_checks.json').write_text(json.dumps(record, indent=2) + '\n')
print('Same-frame native MSL / translated GLSL reflection checks passed:', len(results))
