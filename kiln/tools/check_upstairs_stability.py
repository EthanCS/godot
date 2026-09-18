#!/usr/bin/env python3
"""Measure upper-floor disocclusion dark blocks from the first transition frame."""
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
    temporal_median = np.median(frames, axis=0)
    active = temporal_median > 2.0 / 255.0
    temporal_min = frames.min(axis=0)
    dropout = active & (temporal_min < temporal_median * 0.25)
    delta = np.abs(np.diff(frames, axis=0))

    height = frames.shape[1] // 8 * 8
    width = frames.shape[2] // 8 * 8
    tiles = frames[:, :height, :width].reshape(len(frames), height // 8, 8, width // 8, 8).mean((2, 4))
    tile_median = np.median(tiles, axis=0)
    active_tiles = tile_median > 2.0 / 255.0
    tile_dropout = active_tiles & (tiles.min(axis=0) < tile_median * 0.25)
    per_frame = ((tiles < tile_median * 0.25) & active_tiles).sum((1, 2)) / max(active_tiles.sum(), 1)
    return {
        'mean_luminance': float(frames.mean()),
        'active_pixel_fraction': float(active.mean()),
        'dropout_pixel_fraction': float(dropout.sum() / max(active.sum(), 1)),
        'dropout_tile_fraction': float(tile_dropout.sum() / max(active_tiles.sum(), 1)),
        'maximum_frame_dropout_tile_fraction': float(per_frame.max()),
        'maximum_frame_dropout': int(per_frame.argmax()),
        'temporal_delta_p99': float(np.quantile(delta, 0.99)),
        'temporal_delta_max': float(delta.max()),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    result = {
        'scope': __doc__,
        'raw': metrics(load(args.directory / 'raw_frames')),
        'color': metrics(load(args.directory / 'color_frames')),
    }
    result['checks'] = {
        # A teleport exposes sub-pixel silhouettes with no prior world-space
        # support, so exact zero is not a meaningful acceptance threshold. Large
        # 8x8 allocation blocks are: the original failure was 89% raw / 74%
        # composed, while these bounds retain sensitivity below one visible tile
        # in a thousand for the production image.
        'raw_dark_tile_dropout_below_one_percent': result['raw']['dropout_tile_fraction'] < 0.01,
        'color_dark_tile_dropout_below_point_two_percent': result['color']['dropout_tile_fraction'] < 0.002,
    }
    result['passed'] = all(result['checks'].values())
    (args.directory / 'upstairs_stability_metrics.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    if not result['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
