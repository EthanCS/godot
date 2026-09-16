#!/usr/bin/env python3
"""Fetch the McGuire Sponza benchmark and deterministically merge OBJ material groups.

Assets stay outside version control. Original copyright.txt is retained verbatim.
This conversion only combines faces using the same material; positions, normals,
UVs, materials and textures are unchanged. It avoids Godot's surface-count limit.
"""
from pathlib import Path
import hashlib
import os
import urllib.request
import zipfile

URL = 'https://casual-effects.com/g3d/data10/common/model/crytek_sponza/sponza.zip'
SHA256 = 'da005cbee0be2df2abc8513f3ceb61bcb6f69aac112babcd9c00169a27c2770c'
ROOT = Path(__file__).resolve().parents[1] / 'sponza' / 'assets'
ARCHIVE = Path(os.environ.get('TEMP', '/tmp')) / 'kiln-sponza.zip'

def main():
    if not ARCHIVE.exists():
        urllib.request.urlretrieve(URL, ARCHIVE)
    if hashlib.sha256(ARCHIVE.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('Sponza archive hash mismatch; refusing an unreviewed replacement')
    ROOT.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(ARCHIVE) as archive:
        for item in archive.infolist():
            destination = (ROOT / item.filename).resolve()
            if not destination.is_relative_to(ROOT.resolve()):
                raise RuntimeError('Invalid archive path')
        archive.extractall(ROOT)
    obj = ROOT / 'sponza.obj'
    header, groups, material = [], {}, 'default'
    for line in obj.read_text().splitlines():
        if line.startswith('usemtl '):
            material = line[7:]
        elif line.startswith('f '):
            groups.setdefault(material, []).append(line)
        elif line.startswith(('v ', 'vn ', 'vt ', 'mtllib ')):
            header.append(line)
    for material, faces in groups.items():
        header.extend(['g ' + material, 'usemtl ' + material, *faces])
    obj.write_text('\n'.join(header) + '\n')
    print(f'Sponza: {len(groups)} material groups, {sum(map(len, groups.values()))} faces. {ROOT}')
    print('Original model: Frank Meinl / Crytek; archive modifications: Morgan McGuire.')
    print('Read assets/copyright.txt before redistribution; downloaded payload is not committed.')

if __name__ == '__main__':
    main()
