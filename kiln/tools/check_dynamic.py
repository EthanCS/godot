#!/usr/bin/env python3
"""Check GPU captures against thresholds retained from the source dynamic test.
Run after demo/tests/dynamic_lighting.tscn. Requires NumPy. No thresholds are
fitted to a run: emitter > .015, translation gain > 1.8, old light < .55,
occlusion < .5, hidden contribution < .0001, settled SH bit identical to cold.
"""
import argparse,json
from pathlib import Path
import numpy as np
p=argparse.ArgumentParser();p.add_argument('directory',type=Path);a=p.parse_args()
results={}; measurements={}
def capture(name):
 d=a.directory/name;m=json.loads((d/'metadata.json').read_text());w,h=m['half_width'],m['half_height']
 return {'pos':np.fromfile(d/'position.bin','<f4').reshape(h,w,4),'light':np.fromfile(d/'decoded.bin','<f2').reshape(h,w,4).astype('f4'),'sh':np.fromfile(d/'sh.bin','<f4'),'meta':m}
def avg(c,center,radius=1.3):
 pos=c['pos'];mask=(pos[:,:,3]>.5)&(abs(pos[:,:,1])<.01)&(np.linalg.norm(pos[:,:,:3]-np.array(center),axis=2)<radius)
 assert mask.sum()>=20, f"Only {mask.sum()} receivers at {center}"
 return c['light'][mask,:3].mean(axis=0)
def check(name,test): results[name]=bool(test)
cs={d.name:capture(d.name) for d in sorted(a.directory.iterdir()) if (d/'metadata.json').exists()}
for name,c in cs.items():
 check(name+'_finite',all(np.isfinite(c[k]).all() for k in ['pos','light','sh']))
 measurements[name]={'left':avg(c,[-2,0,.6]).tolist(),'right':avg(c,[2,0,.6]).tolist()}
dark,left,right,blocked,blue=[cs[x] for x in ['00_dark','01_red_left','02_red_right','03_occluded','04_blue']]
l=avg(left,[-2,0,.6]);r=avg(right,[2,0,.6])
check('emission_without_sun_sky_local_lights',l[0]>avg(dark,[-2,0,.6])[0]+.015 and l[0]>l[2]*2)
check('translation_new_position_gain',r[0]>avg(left,[2,0,.6])[0]*1.8)
check('translation_old_position_clears',avg(right,[-2,0,.6])[0]<l[0]*.55)
check('dynamic_occluder',avg(blocked,[2,0,1.55],.55)[0]<avg(right,[2,0,1.55],.55)[0]*.5)
b=avg(blue,[2,0,.6]);check('emission_color_updates',b[2]>b[0]*2 and b[0]<r[0]*.55)
check('off_32_low_residual',np.linalg.norm(avg(cs['06_off_32'],[0,0,0],4))<.005)
check('hidden_emitters_no_ghost',np.linalg.norm(avg(cs['07_off_settled'],[0,0,0],4))<.0001)
check('omni_indirect',avg(cs['08_omni'],[0,0,0],4)[0]>.015)
check('spot_indirect',avg(cs['09_spot'],[0,0,0],4)[2]>.015)
check('continuous_motion_updates',cs['10_motion']['meta']['moving'])
check('settled_equals_cold',np.array_equal(cs['11_settled']['sh'],cs['12_cold']['sh']))
check('static_rebuild_preserves_result',np.array_equal(cs['12_cold']['sh'],cs['13_rebuild']['sh']))
report={'thresholds_origin':'OceanCastle KilnGiDynamicLightingCheck.cs (source SHA in provenance)','checks':results,'measurements':measurements,'passed':all(results.values())}
(a.directory/'checks.json').write_text(json.dumps(report,indent=2))
for name,value in results.items():print(('PASS ' if value else 'FAIL ')+name)
raise SystemExit(0 if report['passed'] else 1)
