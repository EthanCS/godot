#!/usr/bin/env python3
"""Check a GPU grid readback against spatial support, independently of list order.

Checks contiguous offsets, bounds, live entries, duplicates and candidate
completeness against a brute-force surfel query. Run on explicit captures only.
"""
import argparse
import json
from pathlib import Path

import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument('capture', type=Path)
args = parser.parse_args()
root = args.capture
s = np.fromfile(root / 'surfels.bin', '<f4').reshape(-1, 32)
heads = np.fromfile(root / 'cell_heads.bin', '<u4').reshape(-1, 2)
entries = np.fromfile(root / 'cell_links.bin', '<u4')
sums = np.fromfile(root / 'grid_sums.bin', '<u4')
alive = s.view('<u4')[:, 22] != 0
ids = np.flatnonzero(alive)
offsets = heads[:, 1].astype(np.uint64) + sums[np.arange(len(heads)) // 64]
expected = np.cumsum(heads[:, 0], dtype=np.uint64) - heads[:, 0]
assert np.array_equal(offsets, expected), 'non-contiguous GPU prefix offsets'
total = int(heads[:, 0].sum())
assert total <= len(s) * 27 and total <= len(entries)
assert (entries[:total] < len(s)).all() and alive[entries[:total]].all()
for key in np.flatnonzero(heads[:, 0]):
    offset, count = int(offsets[key]), int(heads[key, 0])
    assert len(np.unique(entries[offset:offset + count])) == count, 'duplicate surfel in bucket'

def hash_cell(cell, level):
    x, y, z = (int(v) & 0xffffffff for v in cell)
    h = (x * 73856093 ^ y * 19349663 ^ z * 83492791 ^ level * 2654435761) & 0xffffffff
    h = ((h ^ (h >> 16)) * 0x7feb352d) & 0xffffffff
    h = ((h ^ (h >> 15)) * 0x846ca68b) & 0xffffffff
    return (h ^ (h >> 16)) & (len(heads) - 1)

# Tangential samples exercise support boundaries and all three grid levels;
# brute-force positions do not assume the GPU's insertion-cell algorithm.
rng = np.random.default_rng(71421)
query_count = min(2048, len(ids))
for id in rng.choice(ids, query_count, replace=False):
    n = s[id, 4:7].astype(float)
    tangent = rng.normal(size=3)
    tangent -= n * np.dot(tangent, n)
    tangent /= max(np.linalg.norm(tangent), 1e-20)
    position = s[id, :3] + tangent * s[id, 3] * rng.uniform(0, .999)
    delta = position - s[ids, :3]
    support = (np.einsum('ij,ij->i', delta, delta) < s[ids, 3] ** 2)
    support &= np.abs(np.einsum('ij,ij->i', delta, s[ids, 4:7])) <= np.maximum(.012, s[ids, 3] * .06)
    required = set(ids[support].tolist())
    candidates = set()
    for level in range(3):
        key = hash_cell(np.floor(position / (.25 * (1 << level))).astype(int), level)
        offset, count = int(offsets[key]), int(heads[key, 0])
        candidates.update(entries[offset:offset + count].tolist())
    assert required <= candidates, f'missing spatial support at surfel {id}: {required - candidates}'

result = {'passed': True, 'alive': len(ids), 'references': total,
          'brute_force_queries': query_count, 'maximum_bucket': int(heads[:, 0].max())}
(root / 'grid_checks.json').write_text(json.dumps(result, indent=2))
print(json.dumps(result))
