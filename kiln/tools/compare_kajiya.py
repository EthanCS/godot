#!/usr/bin/env python3
"""Render kajiya's cornell box and the kiln twin at a grid of sun angles and compare.

Drives both engines per angle:
  kajiya: target/release/view --scene cornell_box --frames N --sun-theta/--sun-phi
          -> HDR window mean (raw f32 + .meta.json) + final PNG (display-transformed)
  kiln:   kiln/cornell harness --frames N --sun-theta/--sun-phi -> final PNG
          (kiln HDR mean capture lands with the kajiya lighting port; until then
          kiln HDR metrics are skipped)

Writes per-angle metrics (RMSE/PSNR on HDR mean and final images), side-by-side
composites, and a summary table. Exits nonzero if any angle exceeds --threshold.
"""
from pathlib import Path
import argparse
import json
import math
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parents[2]
KAJIYA = REPO.parent / 'kajiya'
CORNELL = REPO / 'kiln' / 'cornell'
GODOT_EXE = REPO / 'bin' / 'godot.windows.editor.x86_64.mono.console.exe'

# (theta, phi) pairs in kajiya's spherical convention. Elevation grid
# (15/35/60 deg) x azimuth grid (-60/0/+60 deg): phi = 90 - elevation.
TOD_GRID = [
    (theta_deg, 90.0 - elevation_deg)
    for elevation_deg in (15.0, 35.0, 60.0)
    for theta_deg in (-60.0, 0.0, 60.0)
]


def load_hdr_f32(directory: Path) -> tuple[np.ndarray, dict]:
    meta = json.loads((directory / 'kajiya_hdr.f32.meta.json').read_text())
    data = np.fromfile(directory / 'kajiya_hdr.f32', dtype='<f4')
    img = data.reshape(meta['height'], meta['width'], meta['channels'])
    return img, meta


def image_rmse(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.sqrt(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)))


def psnr(rmse: float, peak: float) -> float:
    if rmse <= 1e-12:
        return float('inf')
    return 20.0 * math.log10(peak / rmse)


def load_png(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float32) / 255.0


def run_kajiya(out_dir: Path, theta_deg: float, phi_deg: float, frames: int, width: int, height: int, extra: list[str]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    # A stale view_state.ron would override the camera; the default camera is what we want.
    state_file = KAJIYA / 'view_state.ron'
    state_file.unlink(missing_ok=True)
    cmd = [
        str(KAJIYA / 'target' / 'release' / 'view.exe'),
        '--scene', 'cornell_box',
        '--width', str(width), '--height', str(height),
        '--no-vsync', '--no-debug', '--no-car',
        '--sun-theta', str(math.radians(theta_deg)),
        '--sun-phi', str(math.radians(phi_deg)),
        '--frames', str(frames),
        '--hdr-out', str(out_dir / 'kajiya_hdr.f32'),
        '--screenshot', str(out_dir / 'kajiya_final.png'),
        *extra,
    ]
    subprocess.run(cmd, cwd=KAJIYA, check=True, stdout=subprocess.DEVNULL)


def run_kiln(out_dir: Path, theta_deg: float, phi_deg: float, frames: int, width: int, height: int) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(GODOT_EXE),
        '--path', str(CORNELL),
        '--rendering-driver', 'vulkan',
        '--rendering-method', 'kiln_deferred',
        '--',
        f'--frames={frames}',
        f'--sun-theta={math.radians(theta_deg)}',
        f'--sun-phi={math.radians(phi_deg)}',
        f'--output={out_dir}',
        f'--size={width}x{height}',
        '--no-car',
    ]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    (out_dir / 'color.png').replace(out_dir / 'kiln_final.png')


def side_by_side(images: list[tuple[str, np.ndarray]], path: Path) -> None:
    height = min(img.shape[0] for _, img in images)
    resized = [(name, np.asarray(Image.fromarray((img * 255).astype(np.uint8)).resize(
        (int(img.shape[1] * height / img.shape[0]), height)))) for name, img in images]
    total_width = sum(img.shape[1] + 8 for _, img in resized)
    canvas = Image.new('RGB', (total_width, height + 28), (24, 24, 28))
    x = 0
    draw = ImageDraw.Draw(canvas)
    for name, img in resized:
        canvas.paste(Image.fromarray(img), (x, 28))
        draw.text((x + 4, 6), name, fill=(230, 230, 230))
        x += img.shape[1] + 8
    canvas.save(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frames', type=int, default=256, help='frames per run')
    parser.add_argument('--size', default='960x540', help='WxH render size for both engines')
    parser.add_argument('--output', default=str(CORNELL / 'captures' / 'compare'), help='output root')
    parser.add_argument('--threshold', type=float, default=0.03, help='final-image RMSE fail threshold')
    parser.add_argument('--angles', default='', help='comma list of theta,phi degrees to run (default: full grid)')
    parser.add_argument('--skip-kiln', action='store_true')
    parser.add_argument('--skip-kajiya', action='store_true')
    args = parser.parse_args()

    width, height = (int(v) for v in args.size.split('x'))
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)

    angles = TOD_GRID
    if args.angles:
        angles = [tuple(float(v) for v in item.split(',')) for item in args.angles.split(';')]

    results = []
    for theta_deg, phi_deg in angles:
        label = f'theta{theta_deg:+05.1f}_phi{phi_deg:04.1f}'
        angle_dir = output / label
        print(f'=== {label} (theta={theta_deg} phi={phi_deg}) ===')

        if not args.skip_kajiya:
            run_kajiya(angle_dir, theta_deg, phi_deg, args.frames, width, height, [])
        if not args.skip_kiln:
            run_kiln(angle_dir, theta_deg, phi_deg, args.frames, width, height)

        row = {'label': label, 'theta': theta_deg, 'phi': phi_deg}

        kajiya_hdr, _meta = load_hdr_f32(angle_dir)
        # Mean scene luminance of the lit image, for a relative-error figure.
        row['kajiya_hdr_mean_lum'] = float(kajiya_hdr[..., :3].mean())

        kajiya_final = load_png(angle_dir / 'kajiya_final.png')
        kiln_final = load_png(angle_dir / 'kiln_final.png')
        if kajiya_final.shape != kiln_final.shape:
            raise RuntimeError(f'{label}: resolution mismatch {kajiya_final.shape} vs {kiln_final.shape}')
        row['final_rmse'] = image_rmse(kajiya_final, kiln_final)
        row['final_psnr'] = psnr(row['final_rmse'], 1.0)

        side_by_side(
            [('kajiya', kajiya_final), ('kiln', kiln_final),
             ('|diff|x8', np.clip(np.abs(kajiya_final - kiln_final) * 8.0, 0, 1))],
            angle_dir / 'compare.png',
        )
        results.append(row)
        print(f"    final RMSE {row['final_rmse']:.5f}  PSNR {row['final_psnr']:.2f} dB")

    (output / 'summary.json').write_text(json.dumps(results, indent=2))
    print(f"\n{'label':>18} | {'final RMSE':>10} | {'PSNR dB':>9} | verdict")
    worst = 0.0
    for row in results:
        worst = max(worst, row['final_rmse'])
        verdict = 'PASS' if row['final_rmse'] <= args.threshold else 'FAIL'
        print(f"{row['label']:>18} | {row['final_rmse']:10.5f} | {row['final_psnr']:9.2f} | {verdict}")
    print(f'worst final RMSE {worst:.5f} vs threshold {args.threshold}')
    return 0 if worst <= args.threshold else 1


if __name__ == '__main__':
    sys.exit(main())
