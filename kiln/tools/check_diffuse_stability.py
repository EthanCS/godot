#!/usr/bin/env python3
"""Describe per-frame TOD and moving-camera diffuse stability.

The moving sequence contains genuine parallax and disocclusion, so its values are
reported for regression comparison and visual review rather than assigned a
reference-free correctness threshold.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def load(directory: Path):
    frames = []
    for path in sorted(directory.glob('*.png')):
        rgb = np.asarray(Image.open(path), dtype=np.float32)[..., :3] / 255.0
        frames.append(rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32))
    assert len(frames) == 120, f'{directory}: expected 120 frames, found {len(frames)}'
    return np.stack(frames)


def metrics(frames):
    delta = np.diff(frames, axis=0)
    # Remove broad exposure/illumination changes while retaining disk-scale
    # temporal noise. The decimated low pass avoids a scipy dependency.
    broad = np.repeat(np.repeat(delta[:, ::8, ::8], 8, axis=1), 8, axis=2)
    broad = broad[:, :delta.shape[1], :delta.shape[2]]
    high = delta - broad
    absolute = np.abs(high)
    per_frame = np.quantile(absolute.reshape(len(absolute), -1), 0.99, axis=1)
    return {
        'mean_luminance': float(frames.mean()),
        'temporal_highpass_std': float(high.std()),
        'p99_delta_mean': float(per_frame.mean()),
        'p99_delta_max': float(per_frame.max()),
        'p99_delta_frame': int(per_frame.argmax() + 1),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    stats = json.loads((args.directory / 'stability.json').read_text())
    result = {
        'scope': __doc__,
        'tod': metrics(load(args.directory / 'tod_frames')),
        'motion': metrics(load(args.directory / 'motion_frames')),
        'scheduler': {
            series: {
                'response_max': max(frame['response'] for frame in values),
            }
            for series, values in stats.items()
        },
    }
    (args.directory / 'stability_metrics.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
