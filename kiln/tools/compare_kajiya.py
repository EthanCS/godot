#!/usr/bin/env python3
"""GPU Cornell diagnostics. Outputs are not a claim of complete renderer parity.

Default scene includes the car, default sun is (-4.54, 1.48) radians.
Reference user state is never removed or modified. Both engines render fixed
frame counts; HDR metrics use the same final min(64, frames) averaging window.
"""
from pathlib import Path
import argparse
import json
import math
import os
import subprocess
import tempfile
import shutil
import hashlib
import numpy as np
from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parents[2]
KAJIYA = REPO.parent / 'kajiya'
CORNELL = REPO / 'kiln/cornell'
GODOT_EXE = REPO / 'bin/godot.windows.editor.x86_64.mono.console.exe'
DEFAULT_SUN = (-4.54, 1.48)
CAMERAS = {'front': (0, 1, 8, 0), 'left': (-3, 1, 8, math.atan2(-3, 8)),
           'right': (3, 1, 8, math.atan2(3, 8))}


def srgb_encode(a):
    return np.where(a <= 0.0031308, a * 12.92, 1.055 * np.maximum(a, 0) ** (1 / 2.4) - 0.055)


def load_png(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float32) / 255


def image_rmse(a, b):
    return float(np.sqrt(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)))


def isolated_reference(output, baked=None):
    root = output / 'reference-runtime'
    root.mkdir(exist_ok=True)
    # Kajiya requires the experimental-template DXC shipped alongside its viewer.
    shutil.copyfile(KAJIYA / "dxcompiler.dll", root / "dxcompiler.dll")
    # Junctions expose only renderer input assets; all state/cache writes in the
    # reference working directory stay under this capture's temporary runtime.
    for name in ('assets', 'baked', 'crates/lib/rust-shaders/src', 'crates/lib/rust-shaders-shared/src'):
        link = root / name
        target = baked if name == 'baked' and baked else KAJIYA / name
        link.parent.mkdir(parents=True, exist_ok=True)
        if link.exists() and link.resolve() != target.resolve():
            raise RuntimeError(f'Capture runtime has a different input mapping: {link}. Use a fresh output directory.')
        if not link.exists():
            if os.name == 'nt':
                env = os.environ.copy()
                env['KILN_LINK_PATH'], env['KILN_LINK_TARGET'] = str(link), str(target)
                subprocess.run(['powershell', '-NoProfile', '-Command',
                    'New-Item -ItemType Junction -Path $env:KILN_LINK_PATH -Target $env:KILN_LINK_TARGET | Out-Null'], check=True, env=env)
            else:
                link.symlink_to(target, target_is_directory=True)
    return root


def run_kajiya(runtime, out, sun, camera, frames, width, height, no_car, timeline="static"):
    x, y, z, yaw = camera
    state = f"""(camera_position: ({x}, {y}, {z}),
 camera_rotation: (0, {math.sin(yaw / 2)}, 0, {math.cos(yaw / 2)}),
 vertical_fov: 52, emissive_multiplier: 1,
 sun: (theta: {sun[0]}, phi: {sun[1]}),
 lights: (theta: 1, phi: 1, count: 0, distance: 1.5, multiplier: 10), ev_shift: 0)"""
    (runtime / 'view_state.ron').write_text(state)
    cmd = [str(KAJIYA / 'target/release/view.exe'), '--scene', 'cornell_box',
           '--width', str(width), '--height', str(height), '--no-vsync', '--no-debug',
           f'--sun-theta={sun[0]}', f'--sun-phi={sun[1]}', '--frames', str(frames),
           '--timeline', timeline, '--hdr-out', str(out / 'kajiya_hdr.f32'), '--screenshot', str(out / 'kajiya_raw.png')]
    if no_car: cmd.append('--no-car')
    with (out / 'kajiya.log').open('w') as log:
        subprocess.run(cmd, cwd=runtime, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=240)
    if 'A turbosloth LazyWorker' in (out / 'kajiya.log').read_text(errors='replace'):
        raise RuntimeError(f'Reference render graph failed: {out / "kajiya.log"}')


def run_kiln(out, sun, camera, frames, width, height, no_car, components=False, timeline="static", backend=0, mesh_input="prepared"):
    x, y, z, yaw = camera
    # The editor loads this working directory. Exported release templates load
    # their paired PCK beside the executable and forbid --path overrides.
    cmd = [str(GODOT_EXE), '--rendering-driver', 'vulkan',
           '--rendering-method', 'kiln_deferred', '--', f'--frames={frames}',
           f'--sun-theta={sun[0]}', f'--sun-phi={sun[1]}', f'--output={out}',
           f'--size={width}x{height}', f'--camera-position={x},{y},{z}', f'--camera-yaw={yaw}', '--capture-sequence', f'--timeline={timeline}', f'--query-backend={backend}']
    cmd.append('--mesh-input='+mesh_input)
    if no_car: cmd.append('--no-car')
    if not components: cmd.append('--capture-hdr-only')
    with (out / 'kiln.log').open('w') as log:
        subprocess.run(cmd, cwd=CORNELL, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=240)
    log_text = (out / 'kiln.log').read_text(errors='replace')
    if 'ERROR:' in log_text or 'SCRIPT ERROR:' in log_text:
        raise RuntimeError(f'Kiln GPU/script errors: {out / "kiln.log"}')


def side_by_side(images, path):
    height = images[0][1].shape[0]
    canvas = Image.new('RGB', (sum(a.shape[1] + 8 for _, a in images), height + 28), (24, 24, 28))
    draw, x = ImageDraw.Draw(canvas), 0
    for name, a in images:
        canvas.paste(Image.fromarray(np.uint8(np.clip(a, 0, 1) * 255)), (x, 28))
        draw.text((x + 4, 6), name, fill=(230, 230, 230))
        x += a.shape[1] + 8
    canvas.save(path)


def metrics(out, frames):
    meta = json.loads((out / 'kajiya_hdr.f32.meta.json').read_text())
    h, w = meta['height'], meta['width']
    reference = np.fromfile(out / 'kajiya_hdr.f32', '<f4').reshape(h, w, 4)[..., :3] / meta['hdr_frame_count']
    signals = sorted((out / 'signals').glob('frame*/rtdgi_lighting.bin'))
    if len(signals) != meta['hdr_frame_count']:
        raise RuntimeError(f'HDR window mismatch: kiln {len(signals)}, reference {meta["hdr_frame_count"]}')
    sequence = np.stack([np.fromfile(f, '<f2').reshape(h, w, 4)[..., :3].astype(np.float32) for f in signals])
    if not np.isfinite(sequence).all() or not np.isfinite(reference).all():
        raise RuntimeError(f'Non-finite HDR radiance: {out}')
    kiln = sequence.mean(axis=0)
    surface = np.fromfile(signals[-1].parent / 'surface.bin', '<u4').reshape(h, w, 2)[..., 0] != 0
    row = {'frames': frames, 'hdr_window': meta['hdr_frame_count'], 'surface_pixels': int(surface.sum())}
    for label, mask in [('all', np.ones((h, w), bool)), ('surface', surface), ('background', ~surface)]:
        if not mask.any(): continue
        row[label + '_hdr_rmse'] = image_rmse(reference[mask], kiln[mask])
        denom = max(1e-6, float(np.sqrt(np.mean(reference[mask] ** 2))))
        row[label + '_hdr_relative_rmse'] = row[label + '_hdr_rmse'] / denom
    row['kiln_surface_temporal_std'] = float(sequence[:, surface].std(axis=0).mean())
    row['kajiya_hdr_mean'] = reference.mean(axis=(0, 1)).tolist()
    row['kiln_hdr_mean'] = kiln.mean(axis=(0, 1)).tolist()
    # main_loop.rs quantizes post_combine's LINEAR display RGB directly to PNG.
    # Apply the missing sRGB OETF here; do not change renderer exposure to match it.
    ref_png = srgb_encode(load_png(out / 'kajiya_raw.png'))
    kiln_png = load_png(out / 'color.png')
    row['display_surface_rmse'] = image_rmse(ref_png[surface], kiln_png[surface])
    row['display_all_rmse'] = image_rmse(ref_png, kiln_png)
    side_by_side([('Kajiya (sRGB encoded)', ref_png), ('Kiln', kiln_png),
                  ('absolute difference x4', np.abs(ref_png - kiln_png) * 4)], out / 'display_compare.png')
    # A common, explicit diagnostic transform of both HDR means, independent of display pipelines.
    ref_view = srgb_encode(reference / (1 + reference))
    kiln_view = srgb_encode(kiln / (1 + kiln))
    side_by_side([('Kajiya HDR / (1+HDR)', ref_view), ('Kiln HDR / (1+HDR)', kiln_view),
                  ('absolute difference x4', np.abs(ref_view - kiln_view) * 4)], out / 'hdr_compare.png')
    return row


def reference_variation(out, row, repeats):
    """Compare cross-engine error with independent runs of the reference itself."""
    meta = json.loads((out / 'kajiya_hdr.f32.meta.json').read_text())
    h, w = meta['height'], meta['width']
    dirs = [out] + [out / f'reference_repeat_{i:02d}' for i in range(1, repeats)]
    hdr = [np.fromfile(p / 'kajiya_hdr.f32', '<f4').reshape(h, w, 4)[..., :3] / meta['hdr_frame_count'] for p in dirs]
    display = [srgb_encode(load_png(p / 'kajiya_raw.png')) for p in dirs]
    last = sorted((out / 'signals').glob('frame*/surface.bin'))[-1]
    mask = np.fromfile(last, '<u4').reshape(h, w, 2)[..., 0] != 0
    denom = max(1e-6, float(np.sqrt(np.mean(hdr[0][mask] ** 2))))
    hdr_pairs = [image_rmse(hdr[i][mask], hdr[j][mask]) / denom for i in range(repeats) for j in range(i)]
    display_pairs = [image_rmse(display[i][mask], display[j][mask]) for i in range(repeats) for j in range(i)]
    row['reference_repeats'] = repeats
    row['reference_pairwise_hdr_relative_rmse'] = hdr_pairs
    row['reference_pairwise_display_rmse'] = display_pairs
    row['reference_hdr_variation_rms'] = float(np.sqrt(np.mean(np.square(hdr_pairs))))
    row['reference_display_variation_rms'] = float(np.sqrt(np.mean(np.square(display_pairs))))
    row['within_reference_hdr_variation'] = row['surface_hdr_relative_rmse'] <= 1.25 * row['reference_hdr_variation_rms'] + 0.001
    row['within_reference_display_variation'] = row['display_surface_rmse'] <= 1.25 * row['reference_display_variation_rms'] + 1 / 255
    row['acceptance'] = ('WITHIN_MEASURED_REFERENCE_VARIATION' if row['within_reference_hdr_variation'] and row['within_reference_display_variation']
                         else 'DIFFERENCE_EXCEEDS_REFERENCE_VARIATION')


def main():
    global GODOT_EXE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frames', default='32,128,256', help='comma-separated cold-start frame budgets')
    parser.add_argument('--size', default='960x540')
    parser.add_argument('--output', default=str(Path(tempfile.gettempdir()) / 'kiln-cornell-parity'))
    parser.add_argument('--angles', default='', help='theta,phi pairs in DEGREES separated by semicolons; omit for default sun')
    parser.add_argument('--cameras', default='front,left,right', help='front,left,right')
    parser.add_argument('--no-car', action='store_true', help='diagnostic only; default acceptance includes car')
    parser.add_argument('--components', action='store_true', help='also save intermediate buffers; much larger captures')
    parser.add_argument('--timeline', choices=['static', 'camera', 'relight', 'object', 'combined'], default='static')
    parser.add_argument('--reference-repeats', type=int, default=1, help='independent reference runs per case to measure its own stochastic variation')
    parser.add_argument('--godot', type=Path, default=GODOT_EXE, help='engine executable, including an export template')
    parser.add_argument('--query-backend', type=int, choices=[0, 1, 2], default=0, help='0 auto, 1 compute BVH, 2 prefer hardware')
    parser.add_argument('--mesh-input', choices=['prepared', 'canonical'], default='prepared', help='raw-reference input or explicitly normalized shared diagnostic geometry')
    parser.add_argument('--reference-baked', type=Path, help='required separate baker output for canonical input')
    parser.add_argument('--skip-kajiya', action='store_true')
    parser.add_argument('--skip-kiln', action='store_true')
    parser.add_argument('--threshold', type=float, default=0.03, help='surface HDR relative RMSE diagnostic threshold')
    args = parser.parse_args()
    if args.reference_repeats < 1:
        parser.error('--reference-repeats must be positive')
    if (args.mesh_input == 'canonical') != bool(args.reference_baked):
        parser.error('--mesh-input canonical and --reference-baked must be used together')
    GODOT_EXE = args.godot.resolve()
    width, height = map(int, args.size.split('x'))
    output = Path(args.output).resolve(); output.mkdir(parents=True, exist_ok=True)
    scene_inputs = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted((CORNELL / 'assets' / args.mesh_input).glob('*')) if p.suffix in ('.bin', '.gltf')}
    reference_baked = args.reference_baked.resolve() if args.reference_baked else KAJIYA / 'baked'
    if args.mesh_input == 'canonical':
        manifest = json.loads((reference_baked.parent/'cornell-inputs.json').read_text())
        if manifest['scene_inputs'] != scene_inputs:
            raise RuntimeError('Canonical reference bake does not match the Godot scene inputs. Prepare and bake again.')
    binaries = {'godot': GODOT_EXE, 'kajiya': KAJIYA / 'target/release/view.exe'}
    if GODOT_EXE.name.endswith('.console.exe'):
        binaries['godot_engine'] = GODOT_EXE.with_name(GODOT_EXE.name.replace('.console.exe', '.exe'))
    paired_pack = binaries.get('godot_engine', GODOT_EXE).with_suffix('.pck')
    if paired_pack.is_file():
        binaries['godot_project_pack'] = paired_pack
    (output / 'run.json').write_text(json.dumps({'arguments': {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
        'scene_inputs': {'assets/'+args.mesh_input+'/'+name: digest for name, digest in scene_inputs.items()},
        'reference_scene_inputs': {name: hashlib.sha256((reference_baked/name).read_bytes()).hexdigest()
                                   for name in ['cornell_box.mesh'] + ([] if args.no_car else ['336_lrm.mesh'])},
        'binaries': {k: {'path': str(v), 'sha256': hashlib.sha256(v.read_bytes()).hexdigest()} for k, v in binaries.items()}}, indent=2))
    runtime = isolated_reference(output, args.reference_baked.resolve() if args.reference_baked else None) if not args.skip_kajiya else None
    suns = [DEFAULT_SUN] if not args.angles else [tuple(math.radians(float(v)) for v in item.split(',')) for item in args.angles.split(';')]
    results = []
    for sun_i, sun in enumerate(suns):
        for camera_name in args.cameras.split(','):
            for frames in map(int, args.frames.split(',')):
                label = f'sun{sun_i}_{camera_name}_{frames:04d}'
                out = output / label; out.mkdir(exist_ok=True)
                camera = CAMERAS[camera_name]
                print(f'{label}: sun radians={sun}, camera={camera}', flush=True)
                if not args.skip_kajiya: run_kajiya(runtime, out, sun, camera, frames, width, height, args.no_car, args.timeline)
                if not args.skip_kiln: run_kiln(out, sun, camera, frames, width, height, args.no_car, args.components, args.timeline, args.query_backend, args.mesh_input)
                row = metrics(out, frames)
                row.update(label=label, sun_radians=sun, camera=camera, with_car=not args.no_car, timeline=args.timeline, mesh_input=args.mesh_input)
                row['diagnostic_threshold_met'] = row['surface_hdr_relative_rmse'] <= args.threshold
                # Image error alone cannot establish estimator or convergence equivalence.
                row['acceptance'] = 'NOT_ESTABLISHED'
                if args.reference_repeats > 1:
                    for repeat in range(1, args.reference_repeats):
                        repeated_out = out / f'reference_repeat_{repeat:02d}'
                        repeated_out.mkdir(exist_ok=True)
                        if not args.skip_kajiya:
                            run_kajiya(runtime, repeated_out, sun, camera, frames, width, height, args.no_car, args.timeline)
                    reference_variation(out, row, args.reference_repeats)
                results.append(row)
                (output / 'summary.json').write_text(json.dumps(results, indent=2))
                print(f"surface HDR relative RMSE={row['surface_hdr_relative_rmse']:.5f}; display RMSE={row['display_surface_rmse']:.5f}", flush=True)
    return 0 if all(r['diagnostic_threshold_met'] and (args.reference_repeats == 1 or
        (r['within_reference_hdr_variation'] and r['within_reference_display_variation'])) for r in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
