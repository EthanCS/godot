#!/usr/bin/env python3
"""Copy the kajiya cornell box + car glTF sources into this project, pinned by hash.

These are the exact files `kajiya bake.cmd` consumes (cornell box at scale 2.0,
336_lrm car at scale 0.01). The separate preparation step adapts the imported
scene representation. Asset payloads stay outside version control.
"""
from pathlib import Path
import hashlib
import shutil
from prepare_cornell import main as prepare

# kajiya checkout that provides the reference renderer.
KAJIYA_ROOT = Path(__file__).resolve().parents[3] / 'kajiya'
SOURCES = {
    'assets/meshes/cornell_box/scene.gltf': 'cornell_box/scene.gltf',
    'assets/meshes/cornell_box/scene.bin': 'cornell_box/scene.bin',
    'assets/meshes/cornell_box/license.txt': 'cornell_box/license.txt',
    'assets/meshes/336_lrm/scene.gltf': 'car/scene.gltf',
    'assets/meshes/336_lrm/scene.bin': 'car/scene.bin',
    'assets/meshes/336_lrm/license.txt': 'car/license.txt',
}
ROOT = Path(__file__).resolve().parents[1] / 'cornell' / 'assets'
EXPECTED = Path(__file__).resolve().parents[1] / 'cornell' / 'ASSETS.sha256'


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    if not KAJIYA_ROOT.exists():
        raise RuntimeError(f'kajiya checkout not found at {KAJIYA_ROOT}')
    expected = {name: digest for digest, name in
                (line.split(maxsplit=1) for line in EXPECTED.read_text().splitlines() if line.strip())}
    # Validate every input before copying anything. A changed checkout must not
    # silently redefine the comparison's supposedly pinned scene.
    for source, destination in SOURCES.items():
        if hash_file(KAJIYA_ROOT / source) != expected[destination]:
            raise RuntimeError(f'Asset differs from pinned reference: {source}')
    manifest_lines = []
    ROOT.mkdir(parents=True, exist_ok=True)
    for source, destination in SOURCES.items():
        source_path = KAJIYA_ROOT / source
        if not source_path.exists():
            raise RuntimeError(f'Missing kajiya asset: {source_path}')
        source_hash = hash_file(source_path)
        destination_path = ROOT / destination
        destination_path.parent.mkdir(parents=True, exist_ok=True)
        if not destination_path.exists() or hash_file(destination_path) != source_hash:
            shutil.copyfile(source_path, destination_path)
            print(f'copied {source} -> {destination_path}')
        if hash_file(destination_path) != expected[destination]:
            raise RuntimeError(f'Copied asset failed verification: {destination_path}')
        manifest_lines.append(f'{source_hash}  {destination}')
    (ROOT / 'MANIFEST.sha256').write_text('\n'.join(manifest_lines) + '\n')
    print(f'Verified {len(SOURCES)} files under {ROOT}')
    prepare(ROOT)


if __name__ == '__main__':
    main()
