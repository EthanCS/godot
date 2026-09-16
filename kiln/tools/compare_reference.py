#!/usr/bin/env python3
"""Compare matched original 4.6.2 and native five-view diagnostic captures.
Reports descriptive errors, not fitted acceptance thresholds. Indirect metrics
exclude a three-pixel border around authored emitters, since the source diagnostic
includes material emission and native debug 7 deliberately excludes it.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageFilter
p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, required=True)
p.add_argument('--native', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
def rgb(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float64)/255
results = {}
for view in range(5):
    emission = rgb(a.native/f'reference_{view}_emission.png')
    mask_image = Image.fromarray((emission.max(axis=2)>0).astype('uint8')*255)
    valid_indirect = np.asarray(mask_image.filter(ImageFilter.MaxFilter(7))) == 0
    for mode in ['lit', 'direct', 'albedo', 'indirect']:
        name = f'reference_{view}_{mode}.png'
        source, native = rgb(a.source/name), rgb(a.native/name)
        assert source.shape == native.shape
        error = abs(source-native)
        values = error[valid_indirect] if mode == 'indirect' else error
        results[name] = {
            'mean_abs_display_rgb': float(values.mean()),
            'p95_abs_display_rgb': float(np.quantile(values, .95)),
            'rmse_display_rgb': float(np.sqrt((values**2).mean())),
            'emission_mask_excluded': mode == 'indirect',
            'excluded_pixel_fraction': float(1-valid_indirect.mean()) if mode == 'indirect' else 0,
        }
        # Side-by-side original / native / x8 error, saved only in temp output.
        images = [Image.fromarray((source*255).astype('uint8')), Image.fromarray((native*255).astype('uint8')), Image.fromarray((np.minimum(error*8,1)*255).astype('uint8'))]
        chart = Image.new('RGB', (images[0].width*3, images[0].height))
        for index, image in enumerate(images): chart.paste(image, (index*image.width, 0))
        chart.save(a.output/name)
(a.output/'metrics.json').write_text(json.dumps(results, indent=2))
print(json.dumps(results, indent=2))
