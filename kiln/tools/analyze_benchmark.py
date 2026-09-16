#!/usr/bin/env python3
"""Summarize raw wall-clock/CPU profiles. GPU timestamps never fabricated.
Each row aggregates exactly three repetitions; individual runs remain in CSV/JSON.
"""
import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
parser.add_argument('--require-complete', action='store_true')
args = parser.parse_args()
metadata = json.loads((args.directory / 'metadata.json').read_text())
groups = defaultdict(list)
runs = []
for path in sorted(args.directory.glob('*/run.json')):
    data = json.loads(path.read_text())
    if data['captures_during_timing'] or data['world']['light_overflow']:
        raise ValueError(f'Invalid timing run: {path}')
    stage_samples = defaultdict(list)
    # Engine profile times are offsets from frame begin. Adjacent differences
    # measure CPU submission for each labelled region, never GPU execution.
    for frame in data['profiles']:
        per_frame = defaultdict(float)
        entries = frame['profile']
        for here, following in zip(entries, entries[1:]):
            elapsed = following['cpu_ms'] - here['cpu_ms']
            if elapsed >= 0:
                per_frame[here['name']] += elapsed
        for name, elapsed in per_frame.items():
            stage_samples[name].append(elapsed)
    cpu = {name: statistics.median(values) for name, values in stage_samples.items()}
    world = data['world']
    row = {
        'run': path.parent.name, 'renderer': data['renderer'],
        'lights': data['lights'], 'dense': data['dense'],
        'shadows': data['shadows'], 'gi': data['gi'] != 'off',
        'taa': data['taa'], 'ao': data['ao'],
        'sample_count': len(data['frame_ms']),
        'sample_seconds': data['sample_seconds'][-1] - data['sample_seconds'][0],
        'median_ms': data['0.5'], 'p95_ms': data['0.95'], 'p99_ms': data['0.99'],
        'vram_mib': data['video_memory_bytes'] / 1048576,
        'uploaded_omni': world['uploaded_omni'], 'uploaded_spot': world['uploaded_spot'],
        'uploaded_shadows': world['uploaded_local_shadows'],
        'overflow': world['light_overflow'],
        'rays': world['rays_per_frame'], 'convergence_samples': world['convergence_samples'],
        'gpu_ms': None, 'cpu_stage_ms': cpu,
    }
    runs.append(row)
    groups[(row['lights'], row['dense'], row['shadows'], row['gi'], row['renderer'])].append(row)
expected = len(metadata['conditions']) * 6
if args.require_complete and len(runs) != expected:
    raise ValueError(f'{len(runs)} runs; expected {expected}')
comparisons = []
for lights, dense, shadows, gi in metadata['conditions']:
    compared = {'lights': lights, 'dense': dense, 'shadows': shadows, 'gi': gi}
    for renderer in ['forward_plus', 'kiln_deferred']:
        rows = groups[(lights, dense, shadows, gi, renderer)]
        if len(rows) != 3:
            break
        summary = {'repetitions': 3}
        for field in ['median_ms', 'p95_ms', 'p99_ms', 'vram_mib']:
            values = [r[field] for r in rows]
            summary[field] = statistics.median(values)
            summary[field + '_range'] = [min(values), max(values)]
        summary['minimum_sample_count'] = min(r['sample_count'] for r in rows)
        summary['cpu_stage_ms'] = {name: statistics.median([r['cpu_stage_ms'][name] for r in rows]) for name in rows[0]['cpu_stage_ms'] if all(name in r['cpu_stage_ms'] for r in rows)}
        compared[renderer] = summary
    if 'kiln_deferred' in compared:
        compared['deferred_over_forward'] = compared['kiln_deferred']['median_ms'] / compared['forward_plus']['median_ms']
        comparisons.append(compared)
report = {'metadata': metadata, 'complete': len(runs) == expected, 'run_count': len(runs), 'comparisons': comparisons, 'runs': runs}
(args.directory / 'analysis.json').write_text(json.dumps(report, indent=2))
with (args.directory / 'runs.csv').open('w') as stream:
    keys = [key for key in runs[0] if key != 'cpu_stage_ms'] if runs else []
    writer = csv.DictWriter(stream, fieldnames=keys)
    writer.writeheader()
    writer.writerows({key: row[key] for key in keys} for row in runs)
lines = ['# Controlled performance measurements', '',
         f"{len(runs)}/{expected} runs. Three repetitions per renderer and condition.",
         'Values below are the median of three per-run percentiles, in milliseconds.',
         'GPU stage times are N/A; CPU submission regions and per-run ranges are in analysis.json.', '',
         '| Lights | Layout | Shadows | GI | Forward median / P95 / P99 | Deferred median / P95 / P99 | D/F | VRAM MiB F / D |',
         '| ---: | --- | ---: | --- | --- | --- | ---: | --- |']
for row in comparisons:
    f, d = row['forward_plus'], row['kiln_deferred']
    fmt = lambda x: ' / '.join(f"{x[key]:.3f}" for key in ['median_ms', 'p95_ms', 'p99_ms'])
    lines.append(f"| {row['lights']} | {'dense' if row['dense'] else 'scattered'} | {row['shadows']} | {'on' if row['gi'] else 'off'} | {fmt(f)} | {fmt(d)} | {row['deferred_over_forward']:.3f} | {f['vram_mib']:.1f} / {d['vram_mib']:.1f} |")
(args.directory / 'report.md').write_text('\n'.join(lines) + '\n')
print(f'{len(runs)}/{expected} runs; {len(comparisons)} complete comparisons')
