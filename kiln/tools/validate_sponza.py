#!/usr/bin/env python3
"""Run Sponza acceptance in a real window; headless is used only for asset import."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--engine', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--driver', choices=['vulkan', 'metal', 'd3d12'], default='vulkan')
parser.add_argument('--size', default='1280x720')
parser.add_argument('--software', action='store_true')
args = parser.parse_args()
args.engine = args.engine.resolve()
args.output = args.output.resolve()
args.output.mkdir(parents=True, exist_ok=True)
base = [str(args.engine), '--path', str(ROOT / 'kiln/sponza'), '--rendering-driver', args.driver, '--rendering-method', 'kiln_deferred']

def run(name, command, marker=None):
    path = args.output / (name + '.log')
    print(name, flush=True)
    with path.open('w', encoding='utf-8') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=900)
    output = path.read_text(encoding='utf-8', errors='replace')
    if result.returncode or 'ERROR:' in output or (marker and marker not in output):
        raise RuntimeError(f'{name} failed: {path}')

run('import', base + ['--headless', '--editor', '--import'])
backend = ['--software'] if args.software else []
run('proxy_import', base + ['--script', 'res://tests/import_proxy.gd', '--', '--output=' + str(args.output / 'proxy_import')], '[IMPORT_PROXY] passed')
run('suite', base + ['--', *backend, '--suite', '--size=' + args.size, '--output=' + str(args.output / 'suite')], '[SPONZA_SUITE] completed')
run('analysis', [sys.executable, str(ROOT / 'kiln/tools/check_sponza.py'), str(args.output / 'suite')])
run('lifecycle', base + ['res://proxy_lifecycle.tscn', '--', '--output=' + str(args.output / 'lifecycle')], '[PROXY_LIFECYCLE] passed')
run('tod', base + ['--', *backend, '--tod-suite', '--size=' + args.size, '--output=' + str(args.output / 'tod')], '[SPONZA_TOD] completed')
run('tod_analysis', [sys.executable, str(ROOT / 'kiln/tools/check_tod.py'), str(args.output / 'tod')])
record = {
    'passed': True,
    'engine': str(args.engine),
    'engine_sha256': hashlib.sha256(args.engine.read_bytes()).hexdigest(),
    'driver': args.driver,
    'force_software': args.software,
    'proxy_import': json.loads((args.output / 'proxy_import/checks.json').read_text()),
    'tod': json.loads((args.output / 'tod/checks.json').read_text()),
    'size': args.size,
    'suite': json.loads((args.output / 'suite/checks.json').read_text()),
    'lifecycle': json.loads((args.output / 'lifecycle/checks.json').read_text()),
}
(args.output / 'passed.json').write_text(json.dumps(record, indent=2))
print('Sponza numerical checks passed. Inspect the captures separately; this is not a performance result.')
