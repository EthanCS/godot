#!/usr/bin/env python3
"""Bake the hash-pinned Cornell inputs using Kajiya's mesh-loading conventions.

This is a scene-input adapter, not a rendering correction. The original six
asset files remain unchanged. Only these untextured, static glTF inputs are
supported. Node transforms, including the reference's normal transform, are
baked before Godot imports the comparison mesh.
"""
from pathlib import Path
import hashlib
import json
import re
import numpy as np


def prepare(root: Path, name: str, scale: float):
    source = root / name / 'scene.gltf'
    doc = json.loads(source.read_text())
    if doc.get('skins') or doc.get('animations') or doc.get('textures'):
        raise ValueError('Cornell input adapter supports only the pinned static, untextured assets')
    buffers = [(source.parent / b['uri']).read_bytes() for b in doc['buffers']]

    def accessor(index):
        a = doc['accessors'][index]
        if 'sparse' in a:
            raise ValueError('Sparse input is not supported')
        view = doc['bufferViews'][a['bufferView']]
        dtype = np.dtype({5123: '<u2', 5125: '<u4', 5126: '<f4'}[a['componentType']])
        count = {'SCALAR': 1, 'VEC3': 3}[a['type']]
        offset = view.get('byteOffset', 0) + a.get('byteOffset', 0)
        return np.ndarray((a['count'], count), dtype, buffers[view['buffer']], offset,
                          (view.get('byteStride', dtype.itemsize * count), dtype.itemsize)).copy()

    def multiply(matrix, vectors):
        # Match glam 0.18's ordered float32 multiply/adds. BLAS may reassociate
        # or fuse them, moving the car's nearly coincident faces by an ULP.
        result = vectors[..., 0:1] * matrix[:, 0]
        for axis in range(1, 4):
            result = vectors[..., axis:axis+1] * matrix[:, axis] + result
        return result

    def transform(node):
        if 'matrix' in node:
            return np.array(node['matrix'], 'f4').reshape(4, 4).T
        x, y, z, w = np.array(node.get('rotation', [0, 0, 0, 1]), 'f4')
        result = np.eye(4, dtype='f4')
        # gltf 0.16 forms these terms in this order before T * R * S.
        x2, y2, z2 = x+x, y+y, z+z
        xx, xy, xz = x2*x, x2*y, x2*z
        yy, yz, zz = y2*y, y2*z, z2*z
        sx, sy, sz = x2*w, y2*w, z2*w
        result[:3, :3] = [[1-yy-zz, xy-sz, xz+sy],
                         [xy+sz, 1-xx-zz, yz-sx],
                         [xz-sy, yz+sx, 1-xx-yy]]
        result[:3, :3] *= np.array(node.get('scale', [1, 1, 1]), 'f4')
        result[:3, 3] = node.get('translation', [0, 0, 0])
        return result

    groups = {}
    def visit(index, parent):
        node = doc['nodes'][index]
        matrix = multiply(parent, transform(node).T).T
        if 'mesh' in node:
            for primitive in doc['meshes'][node['mesh']]['primitives']:
                if set(primitive['attributes']) != {'POSITION', 'NORMAL'} or primitive.get('mode', 4) != 4:
                    raise ValueError('Unexpected geometry in pinned Cornell input')
                position = accessor(primitive['attributes']['POSITION'])
                normal = accessor(primitive['attributes']['NORMAL'])
                position = multiply(matrix, np.column_stack((position, np.ones(len(position), 'f4'))))[:, :3]
                # Kajiya mesh.rs LoadGltfScene uses xform * normal, not an
                # inverse-transpose transform. Bake that same scene input.
                normal = multiply(matrix, np.column_stack((normal, np.zeros(len(normal), 'f4'))))[:, :3]
                normal *= 1.0 / np.linalg.norm(normal, axis=1, keepdims=True)
                indices = (accessor(primitive['indices']).reshape(-1) if 'indices' in primitive
                           else np.arange(len(position), dtype='<u4')).astype('<u4').reshape(-1, 3)
                if np.linalg.det(matrix) < 0:
                    indices = indices[:, ::-1]
                group = groups.setdefault(primitive['material'], [])
                group.append((position, normal, indices))
        for child in node.get('children', []):
            visit(child, matrix)

    initial = np.diag(np.array([scale, scale, scale, 1], 'f4'))
    for node in doc['scenes'][doc.get('scene', 0)]['nodes']:
        visit(node, initial)
    output = {'asset': {'version': '2.0', 'generator': 'Kiln pinned Cornell input adapter'},
              'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
              'meshes': [{'primitives': []}], 'materials': doc['materials'],
              'buffers': [], 'bufferViews': [], 'accessors': []}
    payload = bytearray()
    def append(data, component, shape, position=False):
        while len(payload) % 4:
            payload.append(0)
        raw = data.astype('<f4' if component == 5126 else '<u4').tobytes()
        view = len(output['bufferViews'])
        output['bufferViews'].append({'buffer': 0, 'byteOffset': len(payload), 'byteLength': len(raw)})
        payload.extend(raw)
        value = {'bufferView': view, 'componentType': component, 'count': len(data), 'type': shape}
        if position:
            value.update(min=data.min(axis=0).tolist(), max=data.max(axis=0).tolist())
        result = len(output['accessors'])
        output['accessors'].append(value)
        return result
    for material, group in groups.items():
        positions, normals, indices, offset = [], [], [], 0
        for p, n, idx in group:
            positions.append(p); normals.append(n); indices.append(idx + offset); offset += len(p)
        attrs = {'POSITION': append(np.concatenate(positions), 5126, 'VEC3', True),
                 'NORMAL': append(np.concatenate(normals), 5126, 'VEC3')}
        output['meshes'][0]['primitives'].append({'attributes': attrs, 'material': material,
            'indices': append(np.concatenate(indices).reshape(-1), 5125, 'SCALAR')})
    destination = root / 'prepared'
    destination.mkdir(exist_ok=True)
    output['buffers'] = [{'uri': name+'.bin', 'byteLength': len(payload)}]
    # Godot hashes the glTF source for reimport. Include the external payload's
    # hash so a normals-only binary change cannot leave a stale imported mesh.
    output['asset']['extras'] = {'payload_sha256': hashlib.sha256(payload).hexdigest()}
    for path, data in [(destination/(name+'.bin'), payload),
                       (destination/(name+'.gltf'), json.dumps(output, separators=(',', ':')).encode())]:
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)
    config = destination/(name+'.gltf.import')
    text = config.read_text() if config.exists() else '[remap]\nimporter="scene"\nimporter_version=2\ntype="PackedScene"\n\n[params]\n'
    for key, value in [('meshes/ensure_tangents', 'false'), ('meshes/generate_lods', 'false'),
                       ('meshes/force_disable_compression', 'true')]:
        line = key+'='+value
        if re.search('^'+re.escape(key)+'=', text, flags=re.M):
            text = re.sub('^'+re.escape(key)+'=.*$', line, text, flags=re.M)
        else:
            text = text.replace('[params]\n', '[params]\n'+line+'\n')
    if not config.exists() or config.read_text() != text:
        config.write_text(text)
    return {'name': name, 'scale_baked': scale, 'sha256': hashlib.sha256(payload).hexdigest(),
            'triangles': sum(len(idx) for group in groups.values() for _, _, idx in group),
            'source': str(source.relative_to(root))}


def main(root=None):
    root = root or Path(__file__).resolve().parents[1] / 'cornell/assets'
    rows = [prepare(root, 'cornell_box', 2.0), prepare(root, 'car', 0.01)]
    (root/'prepared/provenance.json').write_text(json.dumps(rows, indent=2)+'\n')
    print('Prepared reference mesh inputs:', ', '.join(f"{r['name']} {r['triangles']} triangles" for r in rows))


if __name__ == '__main__':
    main()
