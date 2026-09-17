#!/usr/bin/env python3
"""Compile every native GI stage and validate SPIR-V; outputs stay in temp storage."""
from pathlib import Path
import argparse, subprocess, tempfile, json, hashlib
ROOT = Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser()
parser.add_argument('--output',type=Path,default=Path(tempfile.gettempdir())/'kiln-surfel-review'/'shaders')
args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
subprocess.run(['python',str(ROOT/'kiln/tools/build_gi_shaders.py')],check=True)
source=(ROOT/'servers/rendering/renderer_rd/shaders/kiln_gi.glsl').read_text().replace('#[compute]','').replace('#VERSION_DEFINES','')
import re
stages=list(dict.fromkeys(re.findall(r'^#ifdef STAGE_(\w+)',source,re.M)))
for stage in stages:
    for hardware in ([False,True] if stage in ['SURFEL_GENERATE','SURFEL_TRACE','SURFEL_SPECULAR','QUERY_VALIDATE'] else [False]):
        if stage=='QUERY_VALIDATE' and not hardware: continue
        name=stage.lower()+('_hardware' if hardware else '')
        glsl=args.output/(name+'.comp');spv=args.output/(name+'.spv')
        glsl.write_text(source.replace('#version 460','#version 460\n#define STAGE_'+stage+('\n#define KILN_HARDWARE_RAY_QUERY' if hardware else '')))
        subprocess.run(['glslangValidator','-V','--target-env','vulkan1.2','-o',str(spv),str(glsl)],check=True)
        subprocess.run(['spirv-val','--target-env','vulkan1.2',str(spv)],check=True)
manifest=json.loads((ROOT/'thirdparty/surfelplus/upstream.json').read_text())
for name,expected in manifest['files'].items():
    assert hashlib.sha256((ROOT/'thirdparty/surfelplus/upstream'/name).read_bytes()).hexdigest()==expected,name
print('All native GI shader variants and imported-source hashes passed.')
