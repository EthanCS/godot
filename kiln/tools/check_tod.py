#!/usr/bin/env python3
"""Bounded Sponza sun/sky acceptance; GPU capture, not a performance test.

Predeclared limits: finite raw GI; noon/night mean ratio > 2; noon GI visible gain
> .005; noon/sunset raw difference > .001; stationary RGB std < .005;
32-frame settled/reference RGB MAE < .035. Hardware query mismatch count must be
zero, nearest distance error < .003; optional backend image MAE < .01.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
parser.add_argument('--compare', type=Path)
args = parser.parse_args()
root = args.directory
checks, measurements = {}, {}

def image(name, base=root):
    return np.asarray(Image.open(base / name / 'color.png').convert('RGB'), dtype=float) / 255

def raw(name):
    folder = root / name
    meta = json.loads((folder / 'metadata.json').read_text())
    h, w = meta['half_height'], meta['half_width']
    position = np.fromfile(folder / 'position.bin', '<f4').reshape(h,w,4)
    rgb = np.fromfile(folder / 'decoded.bin', '<f2').reshape(h,w,4)[...,:3].astype('f4')
    checks[name+'_finite'] = bool(np.isfinite(rgb).all() and np.isfinite(position).all())
    if meta['backend'] == 'hardware_ray_query':
        checks[name+'_hardware_queries'] = meta['hardware_query_rays'] == 2048 and meta['hardware_query_hit_rays'] > 100 and meta['hardware_query_mismatches'] == 0 and meta['hardware_query_maximum_error'] < .003
    world = json.loads((folder / 'world.json').read_text())
    lighting = json.loads((folder / 'lighting.json').read_text())
    checks[name+'_sun_sky_isolation'] = world['local_lights'] == 0 and world['dynamic_triangles'] == 0 and not lighting['local_mode']
    checks[name+'_imported_proxy'] = world['imported_proxy_surfaces'] == 25 and world['runtime_proxy_builds'] == 0
    return rgb[position[...,3] > .5].mean(0)

for hour in ['0650','0900','1200','1750','2100']:
    name = 'tod_'+hour+'_on'
    measurements[hour+'_raw_mean'] = raw(name).tolist()
    gain = float((image(name) - image('tod_'+hour+'_off')).mean())
    measurements[hour+'_gi_display_gain'] = gain
    if args.compare:
        error = float(np.abs(image(name) - image(name, args.compare)).mean())
        measurements[hour+'_backend_mae'] = error
        checks[hour+'_backend_agreement'] = error < .01
noon = np.asarray(measurements['1200_raw_mean'])
night = np.asarray(measurements['2100_raw_mean'])
sunset = np.asarray(measurements['1750_raw_mean'])
checks['noon_brighter_than_night'] = noon.mean() > night.mean() * 2
checks['tod_changes_diffuse_transport'] = np.linalg.norm(noon-sunset) > .001
checks['noon_gi_visible'] = measurements['1200_gi_display_gain'] > .005
noise = float(np.stack([image('tod_noise_%02d'%i) for i in range(6)]).std(0).mean())
checks['stationary_noise'] = noise < .005
measurements['stationary_rgb_std'] = noise
settled = float(np.abs(image('tod_settled_32') - image('tod_reference')).mean())
checks['motion_settles'] = settled < .035
measurements['settled32_mae'] = settled
checks = {key: bool(value) for key,value in checks.items()}
result = {'passed': all(checks.values()),'checks': checks,'measurements': measurements}
(root/'checks.json').write_text(json.dumps(result, indent=2))
print(json.dumps(result, indent=2))
raise SystemExit(0 if result['passed'] else 1)
