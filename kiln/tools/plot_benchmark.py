#!/usr/bin/env python3
"""Export actual benchmark curves to PNG/PDF. Requires matplotlib and NumPy.
Run after analyze_benchmark.py. No rendering engine/GPU capture is involved.
"""
import argparse
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
p = argparse.ArgumentParser()
p.add_argument('directory', type=Path)
p.add_argument('--dynamic', type=Path)
a = p.parse_args()
data = json.loads((a.directory/'analysis.json').read_text())
plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 10, 'axes.spines.top': False, 'axes.spines.right': False})
colors = {'forward_plus': '#2475B0', 'kiln_deferred': '#D25437'}
labels = {'forward_plus': 'Forward+', 'kiln_deferred': 'Kiln deferred'}
fig, axes = plt.subplots(2, 2, figsize=(11, 7.5), constrained_layout=True)
for (dense, gi), ax in zip([(False, False), (True, False), (False, True), (True, True)], axes.flat):
    rows = [r for r in data['comparisons'] if r['dense'] == dense and r['gi'] == gi and r['shadows'] == 8]
    for renderer in colors:
        x = np.array([r['lights'] for r in rows])
        y = np.array([r[renderer]['median_ms'] for r in rows])
        lo = np.array([r[renderer]['median_ms_range'][0] for r in rows])
        hi = np.array([r[renderer]['median_ms_range'][1] for r in rows])
        ax.errorbar(x, y, yerr=np.array([y-lo, hi-y]), marker='o', capsize=3, color=colors[renderer], label=labels[renderer])
    ax.set_xticks([32, 128, 512, 1024])
    ax.set_xlabel('Local lights')
    ax.set_ylabel('Frame time (ms)')
    ax.set_title(f"{'Dense overlap' if dense else 'Scattered'} · GI {'on, converged' if gi else 'off'}")
    ax.grid(axis='y', alpha=.2)
    ax.legend(frameon=False)
fig.suptitle('Same-engine pipeline comparison · 1920×1080 · 8 local shadow lights\nAA/AO off · median of three runs · whiskers show run-median range', fontsize=12)
fig.savefig(a.directory/'scaling.png', dpi=180)
fig.savefig(a.directory/'scaling.pdf')
if a.dynamic:
    dynamic = json.loads((a.dynamic/'analysis.json').read_text())
    fig, ax = plt.subplots(figsize=(11, 4.6), constrained_layout=True)
    for renderer in colors:
        for gi in [False, True]:
            paths = sorted(a.dynamic.glob(f'{renderer}_*_G{int(gi)}_R*/run.json'))
            curves = []
            for path in paths:
                run = json.loads(path.read_text())
                times = np.array(run['sample_seconds']); times -= times[0]
                values = np.array(run['frame_ms'])
                bins = np.arange(0, dynamic['metadata']['steady_sample_seconds'] + 1, 1)
                curves.append([np.median(values[(times>=t)&(times<t+1)]) if np.any((times>=t)&(times<t+1)) else np.nan for t in bins[:-1]])
            if curves:
                curves = np.asarray(curves)
                ax.plot(bins[:-1] + .5, np.nanmedian(curves, axis=0), color=colors[renderer], linestyle='-' if gi else '--', label=labels[renderer]+(' · GI on' if gi else ' · GI off'))
    ax.axhline(1000/60, color='#555555', linewidth=1, label='60 FPS budget')
    ax.set_xlabel('Deterministic replay time (seconds)')
    ax.set_ylabel('Median frame time per 1-second bin (ms)')
    ax.set_title('All-dynamic island · 128 lights / 8 local shadows · 1080p · TAA/AO on')
    ax.grid(axis='y', alpha=.2)
    ax.legend(frameon=False, ncol=2)
    fig.savefig(a.dynamic/'timeline.png', dpi=180)
    fig.savefig(a.dynamic/'timeline.pdf')
