#!/usr/bin/env python3
"""Predeclared image thresholds: off32 mean < 1% of lit, off32 maximum < .02;
settled32 versus 280-frame reference mean RGB error < .035 (display range 0..1).
Stationary 16-frame mean RGB standard deviation < .005; settled history median
>= 64 rays and explicit reset maximum <= 4 rays.
These bounds check residuals; they do not establish perceptual quality alone.
"""
from pathlib import Path
import json,sys
import numpy as np
from PIL import Image
p=Path(sys.argv[1] if len(sys.argv)>1 else '/tmp/kiln-temporal')
def im(name):return np.asarray(Image.open(p/(name+'.png')).convert('RGB'),dtype=float)/255
lit=im('lit');off=im('off_32');ref=im('off_reference');a=im('settled_32');b=im('settled_reference')
values={'off32_mean_ratio':float(abs(off-ref).mean()/max(lit.mean(),1e-8)), 'off32_max':float(abs(off-ref).max()),'settled32_mean_error':float(abs(a-b).mean())}
checks={'off32_mean_ratio':values['off32_mean_ratio']<.01,'off32_max':values['off32_max']<.02,'settled32_mean_error':values['settled32_mean_error']<.035}
def incident(name):
 d=p/name
 a=np.fromfile(d/'decoded.bin', '<f2').reshape(-1,4).astype('f4');pos=np.fromfile(d/'position.bin','<f4').reshape(-1,4)
 mask=(pos[:,3]>.5)&(abs(pos[:,1])<.01)
 assert mask.sum()>20
 return a[mask,:3].mean(axis=0)
light=incident('offscreen_lit');dark=incident('offscreen_dark')
values['offscreen_red']=float(light[0]);values['offscreen_dark']=float(np.linalg.norm(dark))
checks['offscreen_geometry_contributes']=light[0]>dark[0]+.001
checks['offscreen_removed_clears']=np.linalg.norm(dark)<.0001
checks['finite_sh']=np.isfinite(np.fromfile(p/'buffers/sh.bin','<f4')).all().item()
sequence=np.stack([im('stationary_%02d'%i) for i in range(16)])
mask=sequence[0].mean(axis=2)>.02
assert mask.sum()>100
values['stationary_mean_rgb_std']=float(sequence.std(axis=0)[mask].mean())
checks['stationary_temporal_variation']=values['stationary_mean_rgb_std']<.005
confidence=np.fromfile(p/'buffers/confidence.bin','<f2').astype('f4')
reset=np.fromfile(p/'history_reset/confidence.bin','<f2').astype('f4')
values['settled_confidence_median']=float(np.median(confidence[confidence>0]))
values['reset_confidence_max']=float(reset.max())
checks['settled_history_accumulates']=values['settled_confidence_median']>=64
checks['reset_history_rejected']=values['reset_confidence_max']<=4
checks={k:bool(v) for k,v in checks.items()}
r={'measurements':values,'checks':checks,'passed':all(checks.values())};(p/'checks.json').write_text(json.dumps(r,indent=2));print(json.dumps(r,indent=2));sys.exit(0 if r['passed'] else 1)
