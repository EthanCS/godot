#!/usr/bin/env python3
"""Validate real-GPU surfel captures. This is not a visual or performance verdict."""
from pathlib import Path
import argparse,json
import numpy as np
from PIL import Image

parser=argparse.ArgumentParser()
parser.add_argument('directory',type=Path)
args=parser.parse_args();root=args.directory
checks={};measurements={};captures={}
for path in sorted(root.iterdir()):
    if not (path/'metadata.json').exists():continue
    m=json.loads((path/'metadata.json').read_text());name=path.name
    w,h=m['width'],m['height']
    gi=np.fromfile(path/'diffuse.bin','<f2').reshape(h,w,4).astype('f4')
    confidence=np.fromfile(path/'confidence.bin','<f2').reshape(h,w).astype('f4')
    surfels=np.fromfile(path/'surfels.bin','<f4').reshape(-1,32)
    integers=surfels.view('<u4');alive=integers[:,22]!=0
    checks[name+'_algorithm']=m['gi_algorithm']=='Surfel GI (SurfelPlus adaptation)'
    checks[name+'_finite']=bool(np.isfinite(gi).all() and np.isfinite(confidence).all() and np.isfinite(surfels[alive,:20]).all() and np.isfinite(surfels[alive,24:]).all())
    checks[name+'_nonnegative']=bool((gi[:,:,:3]>=0).all())
    checks[name+'_capacity']=0<=m['surfel_alive']<=m['surfel_capacity']
    checks[name+'_allocation_count']=int(alive.sum())==m['surfel_alive']
    checks[name+'_normal']=bool(np.allclose(np.linalg.norm(surfels[alive,4:7],axis=1),1,atol=0.001))
    checks[name+'_compute_bvh']=m['compute_bvh_maximum_error']<0.003
    if m['backend']=='hardware_ray_query':
        checks[name+'_ray_query']=m['hardware_query_mismatches']==0 and m['hardware_query_hit_rays']>100
    if (path/'surface.bin').exists():
        ids=np.fromfile(path/'surface.bin','<u4').reshape(h,w,2)
        visible=ids[:,:,0]>0
        checks[name+'_gbuffer_surface']=bool(visible.mean()>0.1 and not (path/'visibility.bin').exists())
        packed=ids[:,:,1][visible]
        oct_xy=np.stack([(packed & 65535).astype('<u2').view('<i2'),(packed >> 16).astype('<u2').view('<i2')],axis=-1).astype(float)/32767.0
        checks[name+'_gbuffer_geometric_normal']=bool(np.isfinite(oct_xy).all() and np.max(np.abs(oct_xy))<=1.001)
        coverage=float((confidence[visible]>0).mean())
    else:visible=np.ones((h,w),bool);coverage=float((confidence>0).mean())
    measurements[name]={'mean_rgb':gi[visible,:3].mean(0).tolist(),'coverage':coverage,'surfels':int(alive.sum()),'rays':m['surfel_rays']}
    captures[name]=gi
    world=json.loads((path/'world.json').read_text())
    checks[name+'_original_scene_geometry']=world.get('ray_geometry')=='original_scene_meshes' and 'proxy_triangles_unique' not in world

def mean(name):return np.array(measurements[name]['mean_rgb'])
def display(name):return np.asarray(Image.open(root/name/'color.png').convert('RGB'),dtype=float)/255.0
if 'multibounce' in captures:
    checks['multibounce_changes_transport']=float(np.abs(captures['multibounce']-captures['two_bounce']).mean())>0.0001
    checks['noon_coverage']=measurements['multibounce']['coverage']>0.90
    checks['odd_resize']=(root/'odd_resize/metadata.json').exists()
    m=json.loads((root/'odd_resize/metadata.json').read_text())
    checks['odd_dimensions']=m['width']==961 and m['height']==541
    checks['instance_debug_changes_output']=float(np.abs(display('debug_13')-display('debug_0')).mean())>0.02
    checks['geometric_normal_debug_changes_output']=float(np.abs(display('debug_14')-display('debug_0')).mean())>0.02
if '00_dark' in captures:
    checks['bvh_brute_force']=json.loads((root/'bvh.json').read_text())['passed']
    checks['dark_no_ambient']=float(np.linalg.norm(mean('00_dark')))<0.0001
    checks['sun_sky_transport']=mean('01_day').mean()>mean('00_dark').mean()+0.001
    checks['tod_changes_transport']=float(np.linalg.norm(mean('01_day')-mean('02_sunset')))>0.001
    checks['local_light_bounce']=mean('03_local').mean()>mean('00_dark').mean()+0.001
    red,blue=mean('04_emission_red'),mean('05_emission_blue')
    checks['emission_red']=red[0]>0.001 and red[0]>red[2]*1.5
    checks['emission_blue']=blue[2]>0.001 and blue[2]>blue[0]*1.5
    checks['off_32_residual']=float(np.linalg.norm(mean('06_off_32')))<0.005
    worlds=[json.loads((root/name/'world.json').read_text()) for name in ['04_emission_red','05_emission_blue']]
    checks['material_update_without_rebuild']=worlds[1]['material_version']>worlds[0]['material_version'] and worlds[1]['geometry_version']==worlds[0]['geometry_version'] and worlds[1]['dynamic_version']==worlds[0]['dynamic_version']
    images=np.stack([display(f'noise_{i:02}') for i in range(8)])
    noise=float(images.std(0).mean());measurements['stationary_display_rgb_std']=noise
    checks['stationary_noise']=noise<0.005
    error=float(np.abs(display('motion_settled_32')-display('07_settled')).mean())
    measurements['motion_settled32_mae']=error;checks['motion_settling']=error<0.035
    checks['camera_motion_coverage']=min(measurements[name]['coverage'] for name in captures if name.startswith('motion_'))>0.85
checks={k:bool(v) for k,v in checks.items()}
record={'passed':bool(checks) and all(checks.values()),'checks':checks,'measurements':measurements}
(root/'checks.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({'passed':record['passed'],'checks':len(checks),'failed':[k for k,v in checks.items() if not v],'measurements':measurements},indent=2))
raise SystemExit(0 if record['passed'] else 1)
