#!/usr/bin/env python3
"""Check light-off recovery and the linear debug view against GPU readbacks."""
import argparse
import json
from pathlib import Path

import numpy as np

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', type=Path)
args = parser.parse_args()
values, checks = {}, {}
for shot in ('day', 'off_8', 'off_32', 'off_128', 'restored_32', 'restored_128', 'linear_view'):
    directory = args.directory / shot
    meta = json.loads((directory / 'metadata.json').read_text())
    w, h = meta['width'], meta['height']
    normal = np.fromfile(directory / 'normal_roughness.bin', '<f2').reshape(h, w, 4)
    y, x = np.indices((h, w))
    floor = (x > .34 * w) & (x < .52 * w) & (y > .80 * h) & (y < .96 * h) & (normal[:, :, 1] > .98)
    assert floor.sum() > .015 * w * h, 'Insufficient matching floor'
    raw = np.fromfile(directory / 'raw.bin', '<f2').reshape(h, w, 4).astype('f4')
    values[shot] = float((raw[floor, :3] * np.array([.2126, .7152, .0722])).sum(1).mean())
    checks[shot + '_finite_nonnegative'] = bool(np.isfinite(raw).all() and (raw >= 0).all())
    checks[shot + '_nrd_off'] = not meta['nrd_active']
    if shot == 'linear_view':
        debug = np.fromfile(directory / 'debug.bin', '<f2').reshape(h, w, 4).astype('f4')
        checks['linear_debug_matches_values'] = bool(np.allclose(debug[:, :, :3], raw[:, :, :3] * meta['surfel_debug_gain'], rtol=.001, atol=1e-6))
checks['off32_below_1pct'] = values['off_32'] < values['day'] * .01
checks['off128_below_0_1pct'] = values['off_128'] < values['day'] * .001
checks['restored128_within_5pct'] = abs(values['restored_128'] / values['day'] - 1) < .05
record = {'passed': all(checks.values()), 'checks': checks, 'floor_mean': values}
(args.directory / 'response_checks.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record, indent=2))
raise SystemExit(0 if record['passed'] else 1)
