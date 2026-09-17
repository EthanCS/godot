#!/usr/bin/env python3
"""Check real-window Sponza reflection captures. Visual review remains separate."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
args = parser.parse_args()
checks, measurements, captures = {}, {}, {}

for path in sorted(args.directory.iterdir()):
    if not (path / 'metadata.json').exists():
        continue
    name = path.name
    m = json.loads((path / 'metadata.json').read_text())
    h, w = m['height'], m['width']
    def read(key):
        return np.fromfile(path / (key + '.bin'), '<f2').reshape(h, w, 4).astype('f4')
    base, fresnel, diffuse = read('specular'), read('fresnel'), read('diffuse')
    checks[name + '_finite'] = all(np.isfinite(a).all() for a in [base, fresnel, diffuse])
    checks[name + '_nonnegative'] = (base >= 0).all() and (fresnel >= 0).all()
    checks[name + '_backend'] = m['backend'] in ['hardware_ray_query', 'compute_software_bvh']
    if m['backend'] == 'hardware_ray_query':
        checks[name + '_query_parity'] = m['hardware_query_mismatches'] == 0
    nr = read('normal_roughness')
    material = np.fromfile(path / 'material.bin', 'u1').reshape(h, w, 4)
    floor = (nr[:, :, 1] > .85) & (np.indices((h, w))[0] > h * .5)
    checks[name + '_floor_visible'] = floor.sum() > w * h * .02
    # Sponza floor is a dielectric with specular=.5 (F0=.04, F90=1).
    reflected = base[:, :, :3] * .04 + fresnel[:, :, :3]
    intensity = reflected.mean(2)[floor]
    peak = float(np.quantile(intensity, .999))
    measurements[name] = {
        'roughness': float(np.median(nr[:, :, 3][floor])),
        'floor_specular': float(np.median(material[:, :, 0][floor]) / 255),
        'floor_pixels': int(floor.sum()),
        'mean_reflection': float(intensity.mean()),
        'peak_reflection': peak,
        'relative_support': float((intensity > peak * .1).mean()),
    }
    captures[name] = (m, reflected, diffuse, floor)
    if (path / 'emission.bin').exists():
        measurements[name]['visible_emissive_pixels'] = int((read('emission')[:, :, :3].max(2) > 1).sum())

def display(name):
    return np.asarray(Image.open(args.directory / name / 'color.png').convert('RGB'), dtype=float) / 255

def metric(name, key):
    return measurements[name][key]

checks['wet_roughness'] = abs(metric('wet_day', 'roughness') - .18) < .005
checks['dry_roughness'] = abs(metric('dry_day', 'roughness') - .85) < .005
checks['specular_toggle_zero'] = np.max(captures['wet_specular_off'][1]) == 0
checks['wet_reflection_nonzero'] = metric('wet_day', 'peak_reflection') > .001
checks['wet_changes_final_image'] = np.abs(display('wet_day') - display('wet_specular_off')).mean() > .0005
checks['zero_f0_dark'] = display('zero_f0')[captures['zero_f0'][3]].mean() < .002
checks['metallic_reflection_brighter'] = display('metallic_reflection')[captures['metallic_reflection'][3]].mean() > display('wet_specular_only')[captures['wet_specular_only'][3]].mean() * 1.3
checks['roughness_peak'] = metric('emitter_rough_06', 'peak_reflection') > metric('emitter_rough_85', 'peak_reflection') * 3
checks['roughness_spread'] = metric('emitter_rough_50', 'relative_support') > metric('emitter_rough_06', 'relative_support') * 1.5
for name, channel in [('emitter_rough_18', 0), ('emitter_blue', 2), ('offscreen_emitter', 2)]:
    rgb = captures[name][1][captures[name][3]].mean(0)
    checks[name + '_reflection_color'] = rgb[channel] > max(rgb[(channel + 1) % 3], rgb[(channel + 2) % 3]) * 4
checks['offscreen_source_absent'] = metric('offscreen_emitter', 'visible_emissive_pixels') == 0
checks['offscreen_reflection_present'] = metric('offscreen_emitter', 'peak_reflection') > .01
noise = np.stack([display('wet_noise_%02d' % i) for i in range(8)]).std(0).mean()
measurements['display_noise_std'] = float(noise)
checks['stationary_noise'] = noise < .005
settling = np.abs(display('reflection_settled_32') - display('reflection_reference')).mean()
measurements['settled32_mae'] = float(settling)
checks['motion_settling'] = settling < .035
checks['light_off_decay'] = metric('reflection_off_32', 'peak_reflection') < .001
checks['gi_off_specular'] = np.max(captures['reflection_gi_off'][1]) == 0
checks['gi_off_diffuse'] = np.max(captures['reflection_gi_off'][2][:, :, :3]) == 0
m = captures['reflection_odd_resize'][0]
checks['odd_resize'] = m['width'] == 961 and m['height'] == 541
for name, (m, _, _, _) in captures.items():
    checks[name + '_deferred'] = m['renderer_path'] == 'G-buffer deferred'
    checks[name + '_textured_hits'] = m.get('texture_pages', 0) > 0
log = args.directory / 'run.log'
if log.exists():
    text = log.read_text(errors='replace')
    checks['clean_runtime'] = all(s not in text for s in ['ERROR:', 'Validation Error', 'VUID-'])
record = {'passed': bool(checks) and all(checks.values()), 'checks': {k: bool(v) for k, v in checks.items()}, 'measurements': measurements}
(args.directory / 'specular_checks.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps({'passed': record['passed'], 'checks': len(checks), 'failed': [k for k, v in checks.items() if not v], 'noise': float(noise), 'settling': float(settling)}, indent=2))
raise SystemExit(0 if record['passed'] else 1)
