#!/usr/bin/env python3
"""Sequential real-GPU Sponza measurements with matched rendering workloads.

Use a production build. GPU profiling and functional capture suites are separate.
The deterministic scene advances one timeline step per frame in moving runs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--engine', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--size', default='1920x1080')
parser.add_argument('--driver', choices=['vulkan', 'metal', 'd3d12'], default='vulkan')
parser.add_argument('--metal-specular', choices=['native', 'translated'], help='Select the reflection implementation for a controlled Metal A/B')
parser.add_argument('--embedded', action='store_true', help='Executable has an embedded Sponza PCK')
parser.add_argument('--full-specular-rate', action='store_true')
mode = parser.add_mutually_exclusive_group()
mode.add_argument('--diffuse-only', action='store_true', help='Sun/sky diffuse checks, no NRD or indirect specular')
mode.add_argument('--compare-nrd', action='store_true', help='Compare cache diffuse with/without NRD, no indirect specular')
mode.add_argument('--compare-reflections', action='store_true', help='Sun/sky roughness sweep, full-rate two-ray specular and NRD controls')
parser.add_argument('--surfel-ray-budget', type=int, help='Primary surfel rays per frame; 0 removes the global cap')
parser.add_argument('--frames', type=int, default=480)
parser.add_argument('--long-frames', type=int, default=3600, help='Extended moving regression; 0 to skip')
parser.add_argument('--warmup', type=int, default=180)
args = parser.parse_args()
assert args.metal_specular is None or args.driver == 'metal'
assert args.frames > args.warmup >= 60
args.output.mkdir(parents=True, exist_ok=True)
engine = args.engine.resolve()
cases = [
    ('deferred-full', 'kiln_deferred', ['--still']),
    ('deferred-diffuse', 'kiln_deferred', ['--still', '--no-specular']),
    ('deferred-off', 'kiln_deferred', ['--still', '--no-gi']),
    ('forward-full', 'forward_plus', ['--still']),
    ('forward-off', 'forward_plus', ['--still', '--no-gi']),
    ('moving-full', 'kiln_deferred', []),
    ('moving-diffuse', 'kiln_deferred', ['--no-specular']),
    ('moving-off', 'kiln_deferred', ['--no-gi']),
    ('deferred-full-repeat', 'kiln_deferred', ['--still']),
    ('forward-full-repeat', 'forward_plus', ['--still']),
]
if args.diffuse_only:
    cases = [
        ('deferred-diffuse', 'kiln_deferred', ['--still']),
        ('deferred-diffuse-repeat', 'kiln_deferred', ['--still']),
        ('deferred-off', 'kiln_deferred', ['--still', '--no-gi']),
        ('moving-diffuse', 'kiln_deferred', []),
        ('moving-off', 'kiln_deferred', ['--no-gi']),
        ('forward-diffuse', 'forward_plus', ['--still']),
        ('forward-off', 'forward_plus', ['--still', '--no-gi']),
    ]
if args.compare_nrd:
    assert args.driver == 'vulkan', 'The current NRD adapter supports Vulkan only'
    cases = [
        ('static-baseline', 'kiln_deferred', ['--still', '--no-nrd']),
        ('static-nrd', 'kiln_deferred', ['--still', '--nrd']),
        ('moving-baseline', 'kiln_deferred', ['--no-nrd']),
        ('moving-nrd', 'kiln_deferred', ['--nrd']),
        ('static-nrd-repeat', 'kiln_deferred', ['--still', '--nrd']),
        ('static-baseline-repeat', 'kiln_deferred', ['--still', '--no-nrd']),
        ('moving-nrd-repeat', 'kiln_deferred', ['--nrd']),
        ('moving-baseline-repeat', 'kiln_deferred', ['--no-nrd']),
        ('static-gi-off', 'kiln_deferred', ['--still', '--no-nrd', '--no-gi']),
        ('moving-gi-off', 'kiln_deferred', ['--no-nrd', '--no-gi']),
    ]
if args.long_frames:
    assert args.long_frames > args.warmup
    cases.append(('long-moving', 'kiln_deferred', ['--nrd'] if args.compare_nrd else []))
if args.compare_reflections:
    assert args.driver == 'vulkan', 'The current NRD adapter supports Vulkan only'
    cases = []
    for roughness in (.06, .18, .45, .85):
        for motion in (False, True):
            for nrd in (False, True):
                name = f'r{round(roughness * 100):02}-' + ('moving' if motion else 'static') + ('-nrd' if nrd else '-baseline')
                options = ['--roughness=' + str(roughness), '--full-specular-rate']
                options += ['--nrd', '--nrd-reference'] if nrd else ['--no-nrd']
                if not motion:
                    options += ['--still']
                cases.append((name, 'kiln_deferred', options))
    for motion in (False, True):
        options = [] if motion else ['--still']
        label = 'moving' if motion else 'static'
        cases.extend([
            (label + '-diffuse-control', 'kiln_deferred', options + ['--nrd', '--no-specular']),
            (label + '-gi-off', 'kiln_deferred', options + ['--no-nrd', '--no-gi']),
            (label + '-separate-specular', 'kiln_deferred', options + ['--nrd', '--nrd-reference', '--nrd-separate-specular']),
            (label + '-nrd-repeat', 'kiln_deferred', options + ['--nrd', '--nrd-reference']),
            (label + '-baseline-repeat', 'kiln_deferred', options + ['--no-nrd', '--full-specular-rate']),
        ])
    if args.long_frames:
        cases.append(('long-moving', 'kiln_deferred', ['--nrd', '--nrd-reference']))
results = {}
for name, method, extra in cases:
    frame_count = args.long_frames if name == 'long-moving' else args.frames
    output = args.output / name
    project = [] if args.embedded else ['--path', str(ROOT / 'kiln/sponza')]
    quality = ['--full-specular-rate'] if args.full_specular_rate else []
    if args.diffuse_only:
        quality += ['--no-specular', '--no-nrd']
    if args.compare_nrd:
        quality += ['--no-specular']
    if args.surfel_ray_budget is not None:
        assert args.surfel_ray_budget >= 0
        quality.append('--surfel-ray-budget=' + str(args.surfel_ray_budget))
    if args.metal_specular:
        quality.append('--metal-' + args.metal_specular + '-specular')
    command = [str(engine), *project,
               '--rendering-driver', args.driver, '--rendering-method', method,
               '--', '--benchmark', '--hardware', '--rays=2', '--specular-rays=2',
               '--size=' + args.size, '--frames=' + str(frame_count),
               '--output=' + str(output.resolve()), *extra, *quality]
    with (args.output / (name + '.log')).open('w', encoding='utf-8') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
    log = (args.output / (name + '.log')).read_text(errors='replace')
    assert not any(s in log for s in ['ERROR:', 'Validation Error', 'VUID-']), name
    data = json.loads((output / 'benchmark.json').read_text())
    assert data['statistics']['backend'] == 'hardware_ray_query', name
    assert data['statistics']['profile_frame'] >= frame_count - 4, (name, 'rendering paused or window occluded')
    if args.compare_nrd:
        stats = data['statistics']
        assert stats['nrd_active'] == ('--nrd' in extra) and stats['nrd_diffuse_rays'] == 0, name
        assert stats['specular_rays'] == 0 and not stats['nrd_same_input_reference'], name
    if args.compare_reflections:
        stats = data['statistics']
        assert stats['nrd_active'] == ('--nrd' in extra) and stats['nrd_diffuse_rays'] == 0, name
        assert stats['specular_rays'] == (0 if '--no-specular' in extra else 2), name
        assert not stats['nrd_checkerboard'] and not stats['nrd_same_input_reference'], name
        assert stats['local_lights'] == 0 and stats['emissive_triangles'] == 0, name
    if args.metal_specular:
        expected = 'handwritten_msl' if args.metal_specular == 'native' else 'translated_glsl'
        assert data['statistics']['specular_implementation'] == expected, name
    assert data['taa'] and data['width'] == int(args.size.split('x')[0])
    frames = np.array(data['frame_ms'][args.warmup - 60:], dtype=float)
    assert len(frames) == frame_count - args.warmup, (name, len(frames))
    result = {'mean_ms': float(frames.mean()), 'median_ms': float(np.median(frames)),
              'p95_ms': float(np.quantile(frames, .95)), 'fps': float(1000 / frames.mean()),
              'maximum_ms': float(frames.max()), 'over_16_67_ms': int((frames > 1000 / 60).sum()),
              'measured_frames': len(frames), 'command': command}
    results[name] = result
    print(name, json.dumps(result), flush=True)
runtime = engine.with_name(engine.name.replace('.console.exe', '.exe'))
record = {'engine': str(runtime), 'engine_sha256': hashlib.sha256(runtime.read_bytes()).hexdigest(),
          'driver': args.driver, 'metal_specular': args.metal_specular,
          'size': args.size, 'warmup_frames': args.warmup, 'results': results}
(args.output / 'results.json').write_text(json.dumps(record, indent=2))
