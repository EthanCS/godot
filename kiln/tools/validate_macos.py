#!/usr/bin/env python3
"""Serial real-GPU acceptance checks. Source baseline capture is a separate tool.
Window rendering is required; only the auxiliary camera-clearance query is headless.
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--engine', type=Path, default=root/'bin/godot.macos.editor.arm64')
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
base = [str(a.engine.resolve()), '--path', str(root/'kiln/demo')]
def run(name, command, expected=None):
    print(name, flush=True)
    with (a.output/(name+'.log')).open('w') as output:
        result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, timeout=600)
    text = (a.output/(name+'.log')).read_text()
    if result.returncode or 'ERROR:' in text or 'SCRIPT ERROR:' in text or (expected and expected not in text):
        raise RuntimeError(f'{name} failed; inspect its log')

run('dynamic', base+['res://tests/dynamic_lighting.tscn', '--', '--output='+str(a.output/'dynamic')], '[KILN_CHECK] captures complete')
run('dynamic-analysis', [sys.executable, str(root/'kiln/tools/check_dynamic.py'), str(a.output/'dynamic')])
run('temporal', base+['res://tests/temporal.tscn', '--', '--output='+str(a.output/'temporal')], '[KILN_TEMPORAL] captures complete')
run('temporal-analysis', [sys.executable, str(root/'kiln/tools/check_temporal.py'), str(a.output/'temporal')])
run('shadow-contact', base+['res://tests/shadow_contact.tscn'], '[KILN_SHADOW]')
run('material-admission', base+['res://tests/admission.tscn'], '[KILN_ADMISSION] passed')
for method in ['forward_plus', 'kiln_deferred']:
    run('material-chart-'+method, base+['--rendering-method', method, 'res://tests/material_contract.tscn'])
run('reload', base+['--script', 'res://tests/reload.gd'], '[KILN_RELOAD] cycle 2')
run('camera-clearance', base+['--headless', '--script', 'res://tests/camera_path.gd'], '[KILN_CAMERA_PATH]')
for name, path in {'shadow': '/tmp/kiln-shadow-contact/checks.json', 'admission': '/tmp/kiln-admission.json', 'reload': '/tmp/kiln-reload.json', 'camera': '/tmp/kiln-camera-path.json'}.items():
    data = json.loads(Path(path).read_text())
    assert data['passed'], name
    shutil.copy2(path, a.output/(name+'.json'))
(a.output/'passed.json').write_text(json.dumps({'passed': True, 'engine': str(a.engine), 'checks': ['dynamic', 'temporal', 'shadow', 'admission', 'material-chart-both', 'reload', 'camera']}, indent=2))
print('All checks passed', flush=True)
