#!/usr/bin/env python3
"""Serial real-window Forward+/Kiln A/B. No image or buffer readbacks.
Runs three repeats per condition; optional --quick tests one representative pair.
Full matrix: 32/128/512/1024 lights, scattered/dense, 0/8 shadows, GI off/on.
AO is off in both paths to isolate direct/GI scaling; dynamic showcase includes it.
"""
import argparse,itertools,json,subprocess,time
from pathlib import Path
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--quick',action='store_true');p.add_argument('--duration',type=float,default=12);p.add_argument('--warmup',type=float,default=5);p.add_argument('--engine',type=Path,default=root/'bin/godot.macos.editor.arm64');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
conditions=list(itertools.product([32,128,512,1024],[False,True],[0,8],[False,True]))
if a.quick:conditions=[(128,False,8,True)]
rows=[]
for lights,dense,shadows,gi in conditions:
 for repeat in range(3):
  for renderer in ['forward_plus','kiln_deferred'] if repeat%2==0 else ['kiln_deferred','forward_plus']:
   name=f'{renderer}_L{lights}_D{int(dense)}_S{shadows}_G{int(gi)}_R{repeat}';d=a.output/name;d.mkdir(exist_ok=True)
   cmd=[str(a.engine),'--path',str(root/'kiln/demo'),'--rendering-method',renderer,'--disable-vsync','--','--size=1920x1080','--no-aa','--no-ao','--no-hud','--profile','--benchmark',f'--lights={lights}',f'--shadows={shadows}','--view=0','--time=4',f'--duration={a.duration}',f'--warmup={a.warmup}',f'--report-dir={d}']
   if dense:cmd.append('--dense')
   if not gi:cmd.append('--no-gi')
   print(name,flush=True)
   with (d/'engine.log').open('w') as log:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,timeout=180)
   text=(d/'engine.log').read_text()
   if r.returncode or 'ERROR:' in text or 'SCRIPT ERROR:' in text:raise RuntimeError(f'{name}: failed; inspect engine.log')
   data=json.loads((d/'run.json').read_text());assert not data['captures_during_timing']
   assert data['world']['light_overflow']==0
   row={k:v for k,v in data.items() if k not in ['frame_ms','profiles','sample_seconds']};row['repeat']=repeat;row['name']=name;rows.append(row)
   (a.output/'summary.json').write_text(json.dumps(rows,indent=2))
print(f'{len(rows)} runs complete',flush=True)
