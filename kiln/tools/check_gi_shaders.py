#!/usr/bin/env python3
"""Compile and validate Kiln's software/hardware SPIR-V, optionally NRD adapters."""
import argparse
import ast
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HARDWARE_STAGES = {
    'KILN_SURFEL_TRACE', 'KILN_RESTIR_TRACE', 'KILN_LIGHT',
    'KILN_RTR_TRACE', 'KILN_SHADOW_TRACE', 'KILN_WRC_TRACE',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path(tempfile.gettempdir()) / 'kiln-shaders')
    parser.add_argument('--include-nrd', action='store_true', help='Also validate retained, inactive NRD adapter kernels')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(ROOT / 'kiln/tools/build_gi_shaders.py')], check=True)
    source = (ROOT / 'servers/rendering/renderer_rd/shaders/kiln_gi.glsl').read_text()
    source = source.replace('#[compute]', '').replace('#VERSION_DEFINES', '')
    generator = ast.parse((ROOT / 'kiln/tools/build_gi_shaders.py').read_text())
    stages = next([s.upper() for s in ast.literal_eval(node.value)] for node in generator.body
                  if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'STAGES' for t in node.targets))
    assert set(stages) == set(re.findall(r'^#ifdef STAGE_(\w+)', source, re.M))
    cpp = (ROOT / 'servers/rendering/renderer_rd/kiln/kiln_gi.cpp').read_text()
    registered = re.findall(r'"([A-Z_0-9]+)"', re.search(r'const char \*stages\[\] = \{(.*?)\};', cpp, re.S).group(1))
    assert stages == registered, 'Shader source/engine stage order differs'
    hardware = re.findall(r'"([A-Z_0-9]+)"', re.search(r'for \(const char \*stage : \{(.*?)\}', cpp, re.S).group(1))
    assert set(hardware) == HARDWARE_STAGES, 'Hardware variant coverage differs'
    kernels = []
    for stage in stages:
        for hardware in ([False, True] if stage in HARDWARE_STAGES else [False]):
            defines = '\n#define STAGE_' + stage + ('\n#define KILN_HARDWARE_RAY_QUERY' if hardware else '')
            kernels.append((stage.lower() + ('_hardware' if hardware else ''), source.replace('#version 460', '#version 460' + defines)))
    if args.include_nrd:
        nrd = ROOT / 'servers/rendering/renderer_rd/kiln/nrd/sources'
        for path in sorted(nrd.glob('*.comp')):
            code = path.read_text()
            while '// @' in code:
                previous = code
                for include in nrd.glob('*.inc'):
                    code = code.replace('// @' + include.stem.upper() + '@', include.read_text())
                code = code.replace('// @PROJECT_SKY@', (ROOT / 'kiln/demo/rendering/sky/sky_radiance.gdshaderinc').read_text())
                assert code != previous, path.name + ': unresolved include'
            kernels.append((path.stem, code))
    for name, code in kernels:
        glsl = args.output / (name + '.comp')
        spirv = glsl.with_suffix('.spv')
        glsl.write_text(code)
        subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.2', '-o', str(spirv), str(glsl)], check=True)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(spirv)], check=True)
    print(f'{len(kernels)} shader variants compiled and passed SPIR-V validation.')


if __name__ == '__main__':
    main()
