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
parser.add_argument('--metal-specular', choices=['native', 'translated'])
parser.add_argument('--metal-specular-validate', action='store_true')
args = parser.parse_args()
assert args.metal_specular is None or args.driver == 'metal'
assert not args.metal_specular_validate or args.metal_specular == 'native'
args.engine = args.engine.resolve()
args.output = args.output.resolve()
args.output.mkdir(parents=True, exist_ok=True)
base = [str(args.engine), '--path', str(ROOT / 'kiln/sponza'), '--rendering-driver', args.driver, '--rendering-method', 'kiln_deferred']
if args.driver == 'vulkan':
    base.append('--gpu-validation')

def run(name, command, marker=None):
    path = args.output / (name + '.log')
    print(name, flush=True)
    with path.open('w', encoding='utf-8') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=900)
    output = path.read_text(encoding='utf-8', errors='replace')
    if result.returncode or 'ERROR:' in output or 'Validation Error' in output or (marker and marker not in output):
        raise RuntimeError(f'{name} failed: {path}')

run('import', base + ['--headless', '--editor', '--import'])
backend = ['--software'] if args.software else []
if args.metal_specular:
    backend.append('--metal-' + args.metal_specular + '-specular')
if args.metal_specular_validate:
    backend.append('--metal-specular-validate')
run('geometry_import', base + ['--script', 'res://tests/import_geometry.gd', '--', '--output=' + str(args.output / 'geometry_import')], '[IMPORT_GEOMETRY] passed')
run('comparison', base + ['--', *backend, '--surfel-suite', '--size=' + args.size, '--output=' + str(args.output / 'comparison')], '[SPONZA_SURFEL_SUITE] completed')
run('comparison_analysis', [sys.executable, str(ROOT / 'kiln/tools/check_surfel.py'), str(args.output / 'comparison')])
run('suite', base + ['--', *backend, '--suite', '--size=' + args.size, '--output=' + str(args.output / 'suite')], '[SPONZA_SUITE] completed')
run('analysis', [sys.executable, str(ROOT / 'kiln/tools/check_sponza.py'), str(args.output / 'suite')])
run('specular', base + ['--', *backend, '--specular-suite', '--size=' + args.size, '--output=' + str(args.output / 'specular')], '[SPONZA_SPECULAR] completed')
run('specular_analysis', [sys.executable, str(ROOT / 'kiln/tools/check_specular.py'), str(args.output / 'specular')])
run('lifecycle', base + ['res://scene_geometry_lifecycle.tscn', '--', '--output=' + str(args.output / 'lifecycle')], '[SCENE_GEOMETRY_LIFECYCLE] passed')
run('tod', base + ['--', *backend, '--tod-suite', '--size=' + args.size, '--output=' + str(args.output / 'tod')], '[SPONZA_TOD] completed')
run('tod_analysis', [sys.executable, str(ROOT / 'kiln/tools/check_tod.py'), str(args.output / 'tod')])
if args.metal_specular:
    for path in (p for suite in ['comparison', 'suite', 'specular', 'tod'] for p in (args.output / suite).rglob('metadata.json')):
        capture = json.loads(path.read_text())
        native = args.metal_specular == 'native' and capture['backend'] == 'hardware_ray_query'
        assert capture['specular_implementation'] == ('handwritten_msl' if native else 'translated_glsl'), path
if args.metal_specular_validate:
    run('metal_specular_parity', [sys.executable, str(ROOT / 'kiln/tools/check_metal_specular.py'), str(args.output)])
runtime_engine = args.engine
if args.engine.name.endswith('.console.exe'):
    runtime_engine = args.engine.with_name(args.engine.name.removesuffix('.console.exe') + '.exe')
record = {
    'passed': True,
    'engine': str(args.engine),
    'engine_sha256': hashlib.sha256(args.engine.read_bytes()).hexdigest(),
    'runtime_engine': str(runtime_engine),
    'runtime_engine_sha256': hashlib.sha256(runtime_engine.read_bytes()).hexdigest(),
    'driver': args.driver,
    'force_software': args.software,
    'metal_specular': args.metal_specular,
    'metal_specular_validate': args.metal_specular_validate,
    'gi_algorithm': 'Surfel GI (SurfelPlus adaptation)',
    'comparison': json.loads((args.output / 'comparison/checks.json').read_text()),
    'geometry_import': json.loads((args.output / 'geometry_import/checks.json').read_text()),
    'specular': json.loads((args.output / 'specular/specular_checks.json').read_text()),
    'tod': json.loads((args.output / 'tod/checks.json').read_text()),
    'size': args.size,
    'suite': json.loads((args.output / 'suite/checks.json').read_text()),
    'lifecycle': json.loads((args.output / 'lifecycle/checks.json').read_text()),
}
(args.output / 'passed.json').write_text(json.dumps(record, indent=2))
print('Sponza numerical checks passed. Inspect the captures separately; this is not a performance result.')
