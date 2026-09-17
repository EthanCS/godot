#!/usr/bin/env python3
"""Sequential real-GPU, equal-quality Sponza measurements without readbacks.

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
parser.add_argument('--embedded', action='store_true', help='Executable has an embedded Sponza PCK')
parser.add_argument('--full-specular-rate', action='store_true')
parser.add_argument('--frames', type=int, default=480)
parser.add_argument('--long-frames', type=int, default=3600, help='Extended moving regression; 0 to skip')
parser.add_argument('--warmup', type=int, default=180)
args = parser.parse_args()
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
if args.long_frames:
    assert args.long_frames > args.warmup
    cases.append(('long-moving', 'kiln_deferred', []))
results = {}
for name, method, extra in cases:
    frame_count = args.long_frames if name == 'long-moving' else args.frames
    output = args.output / name
    project = [] if args.embedded else ['--path', str(ROOT / 'kiln/sponza')]
    quality = ['--full-specular-rate'] if args.full_specular_rate else []
    command = [str(engine), *project,
               '--rendering-driver', 'vulkan', '--rendering-method', method,
               '--', '--benchmark', '--hardware', '--rays=2', '--specular-rays=2',
               '--size=' + args.size, '--frames=' + str(frame_count),
               '--output=' + str(output.resolve()), *extra, *quality]
    with (args.output / (name + '.log')).open('w', encoding='utf-8') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
    log = (args.output / (name + '.log')).read_text(errors='replace')
    assert not any(s in log for s in ['ERROR:', 'Validation Error', 'VUID-']), name
    data = json.loads((output / 'benchmark.json').read_text())
    assert data['statistics']['backend'] == 'hardware_ray_query', name
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
          'size': args.size, 'warmup_frames': args.warmup, 'results': results}
(args.output / 'results.json').write_text(json.dumps(record, indent=2))
