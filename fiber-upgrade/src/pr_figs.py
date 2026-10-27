"""Figures for the PR adding scripts/upgrade_vc3d_fibers.py."""
import json, pickle, sys
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.collections import LineCollection
from matplotlib.patches import FancyBboxPatch

import figs
from figs import DARK, LIGHT, MONO, rot, render, frames_to_gif

store = pickle.load(open('fig/store.pkl', 'rb'))
res = figs.res
OUT = figs.OUT
INGESTED = '#3987e5'


def draw_store(ax, scroll, pal, az, el, lw=0.8, center=None):
    lines = store[scroll]['lines']
    allp = np.concatenate(lines)
    c = allp.mean(0) if center is None else center
    segs = [rot(p - c, az, el)[0] for p in lines]
    ax.add_collection(LineCollection(segs, colors=INGESTED, linewidths=lw, alpha=0.8, capstyle='round'))
    e = store[scroll]['edges']
    a, _ = rot(e[:, 0] - c, az, el); b, _ = rot(e[:, 1] - c, az, el)
    ax.scatter(a[:, 0], a[:, 1], s=14, color=pal['ink'], edgecolors=pal['bg'], linewidths=0.7, zorder=5)
    return rot(allp - c, az, el)[0]


def store_figure():
    pal = DARK
    fig = plt.figure(figsize=(16, 10.5), dpi=110, facecolor=pal['bg'])
    boxes = {'PHerc0139': [0.02, 0.53, 0.68, 0.36], 'PHerc0332': [0.02, 0.1, 0.68, 0.36]}
    for scroll, box in boxes.items():
        ax = fig.add_axes(box); ax.set_facecolor(pal['bg'])
        r = draw_store(ax, scroll, pal, 35, 10)
        lo, hi = np.percentile(r, 0.3, 0), np.percentile(r, 99.7, 0)
        pad = (hi - lo) * 0.03
        ax.set_xlim(lo[0] - pad[0], hi[0] + pad[0]); ax.set_ylim(lo[1] - pad[1], hi[1] + pad[1])
        ax.set_aspect('equal', anchor='W'); ax.axis('off')
        tx, ty = 0.72, box[1] + box[3]
        n = len(store[scroll]['lines']); m = len(store[scroll]['edges'])
        fig.text(tx, ty, scroll, color=pal['ink'], fontsize=20, fontweight='semibold', va='top')
        fig.text(tx, ty - 0.045, f'{len(figs.data[scroll])} published fiber files', color=pal['ink2'], fontsize=13.5, va='top')
        fig.text(tx, ty - 0.1, 'in the store after the script:', color=pal['ink2'], fontsize=13.5, va='top')
        fig.text(tx, ty - 0.14, f'{n} fibers', color=pal['ink'], fontsize=18, fontweight='bold', va='top')
        fig.text(tx, ty - 0.185, f'{m} branch links', color=pal['ink'], fontsize=18, fontweight='bold', va='top')
        fig.text(tx, ty - 0.24, 'as published: 0 and 0', color=pal['ink2'], fontsize=13.5, va='top')
    fig.text(0.02, 0.965, 'The published fiber annotations, read back from an object store', color=pal['ink'],
             fontsize=19, fontweight='semibold', va='top')
    fig.text(0.02, 0.925, 'Each fiber\'s line_points as stored, scroll axis left to right. White dots: one end of each '
             'stored fiber_branch edge. Built with upgrade_vc3d_fibers.py, then vc_object_ingest.', color=pal['ink2'],
             fontsize=12.5, va='top')
    fig.text(0.98, 0.02, 'Fiber annotations: Vesuvius Challenge open data, CC BY-NC 4.0', color=pal['ink2'],
             fontsize=10.5, ha='right')
    fig.savefig(OUT + 'pr-store.png', facecolor=pal['bg'])
    plt.close(fig)


def before_after():
    pal = LIGHT
    series = [('as published', 'published', pal['v4_link']), ('after upgrade_vc3d_fibers.py', 'upgraded', pal['v3']),
              ('relabeled as version 3 (reference)', 'as v3', pal['v4_nolink'])]
    vals = {'0139': {'published': (0, 0), 'upgraded': (620, 634), 'as v3': (res['0139 as v3']['fibers'], res['0139 as v3']['edges_imported'])},
            '0332': {'published': (0, 0), 'upgraded': (255, 208), 'as v3': (res['0332 as v3']['fibers'], res['0332 as v3']['edges_imported'])}}
    fig, axes = plt.subplots(1, 2, figsize=(14, 5.8), dpi=110, facecolor=pal['bg'])
    for mi, (ax, title) in enumerate(zip(axes, ['fibers in the store', 'branch links in the store'])):
        ax.set_facecolor(pal['bg'])
        for i, s in enumerate(['0139', '0332']):
            for j, (label, key, col) in enumerate(series):
                v = vals[s][key][mi]
                x = i + (j - 1) * 0.27
                ax.bar(x, v, 0.25, color=col)
                ax.text(x, v + 10, str(v), ha='center', va='bottom', fontsize=13, fontweight='bold', color=pal['ink'])
        ax.set_xticks([0, 1], ['PHerc0139', 'PHerc0332'], fontsize=14, color=pal['ink'])
        ax.set_ylim(0, 720)
        ax.yaxis.grid(True, color=pal['grid'], lw=0.8); ax.set_axisbelow(True)
        for sp in ax.spines.values():
            sp.set_visible(False)
        ax.tick_params(axis='y', colors=pal['ink2'], labelsize=12, length=0); ax.tick_params(axis='x', length=0)
        ax.set_title(title, loc='left', fontsize=14.5, color=pal['ink'], fontweight='semibold')
    handles = [plt.Rectangle((0, 0), 1, 1, color=c) for _, _, c in series]
    fig.legend(handles, [l for l, _, _ in series], loc='upper right', bbox_to_anchor=(0.985, 0.985), ncol=3,
               frameon=False, fontsize=12, labelcolor=pal['ink'])
    fig.suptitle('vc_object_ingest on villa main 39a8cf3', x=0.012, ha='left', y=0.975, fontsize=16,
                 fontweight='semibold', color=pal['ink'])
    fig.text(0.012, 0.02, 'After the script the store holds the same fibers, metadata, geometry and links as the '
             'version 3 reference, compared field by field.', fontsize=11.5, color=pal['ink2'])
    fig.subplots_adjust(left=0.05, right=0.985, top=0.8, bottom=0.14, wspace=0.18)
    fig.savefig(OUT + 'pr-before-after.png', facecolor=pal['bg'])
    plt.close(fig)


def json_card(fig, x, y, w, h, title, rows, pal, accent, alpha=1.0):
    ax = fig.add_axes([x, y, w, h]); ax.axis('off'); ax.set_xlim(0, 1); ax.set_ylim(0, 1)
    ax.add_patch(FancyBboxPatch((0, 0), 1, 1, boxstyle='round,pad=0,rounding_size=0.03', fc='white', ec=pal['grid'], lw=1.4))
    ax.add_patch(FancyBboxPatch((0, 0.985), 1, 0.015, boxstyle='square,pad=0', fc=accent, ec='none'))
    ax.text(0.04, 0.91, title, fontsize=12.5, fontweight='semibold', color=pal['ink'], va='top', family=MONO)
    yy = 0.78
    for text, style, a in rows:
        color = {'ok': pal['ink'], 'dim': pal['ink2'], 'add': '#1a7f37', 'gone': '#c4302b'}[style]
        t = ax.text(0.05, yy, text, family=MONO, fontsize=11.5, color=color, va='top', alpha=a,
                    fontweight='bold' if style in ('add', 'gone') else 'normal')
        if style == 'gone':
            ax.plot([0.05, 0.05 + 0.0171 * len(text)], [yy - 0.04, yy - 0.04], color='#c4302b', lw=1.6, alpha=a)
        yy -= 0.1
    return ax


def upgrade_gif():
    pal = LIGHT
    files = {f['name']: f for f in figs.data['PHerc0332']}
    lt = files['lt_20261016T073323561_000068.json']
    kb = files[lt['branches'][0]['branch_file']]
    # Files are read in name order, so kb's declaration is kept and lt's repeat is dropped.
    kept = next(x for x in kb['branches'] if x['branch_file'] == lt['name'])
    repeat = lt['branches'][0]
    a, b, peer_name = kb, lt, lt['name']
    uuid_b = lt['uuid']
    frames = []
    steps = [0.0] * 10 + [0.25, 0.5, 0.75] + [1.0] * 14
    for t in steps:
        fig = plt.figure(figsize=(12, 6.2), dpi=100, facecolor=pal['bg'])
        fig.text(0.03, 0.95, 'What upgrade_vc3d_fibers.py changes in one link', fontsize=17, fontweight='semibold',
                 color=pal['ink'], va='top')
        state = 'as VC3D saved it' if t < 0.5 else 'in the copy ingest reads'
        fig.text(0.03, 0.885, state, fontsize=13, color=pal['ink2'], va='top')
        rows_a = [('"version": 4,', 'ok', 1), ('"branches": [{', 'ok', 1),
                  (f'  "branch_file": "{peer_name[:28]}...",', 'dim', 1),
                  (f'  "control_point_index": {kept["control_point_index"]},', 'dim', 1),
                  (f'  "branch_control_point_index": {kept["branch_control_point_index"]},', 'dim', 1),
                  (f'  "target_uuid": "{uuid_b[:18]}..."', 'add', t), ('}]', 'ok', 1)]
        rows_b = [('"version": 4,', 'ok', 1), ('"branches": [{', 'ok', 1),
                  (f'  "branch_file": "{kb["name"][:28]}...",', 'gone' if t >= 0.5 else 'dim', 1 - 0.55 * t),
                  (f'  "control_point_index": {repeat["control_point_index"]},', 'gone' if t >= 0.5 else 'dim', 1 - 0.55 * t),
                  (f'  "branch_control_point_index": {repeat["branch_control_point_index"]},', 'gone' if t >= 0.5 else 'dim', 1 - 0.55 * t),
                  ('}, ...]', 'ok', 1)]
        json_card(fig, 0.03, 0.3, 0.455, 0.52, a['name'], rows_a, pal, pal['v4_link'])
        json_card(fig, 0.515, 0.3, 0.455, 0.52, lt['name'], rows_b, pal, pal['v4_link'])
        if t >= 0.5:
            fig.text(0.03, 0.22, '+ target_uuid: the other file\'s uuid, or the name-based UUID ingest gives a file without one',
                     fontsize=12, color='#1a7f37', va='top', alpha=t)
            fig.text(0.03, 0.16, '- the same link declared again from the other end: checked against the first (1e-9), then dropped',
                     fontsize=12, color='#c4302b', va='top', alpha=t)
        else:
            fig.text(0.03, 0.22, 'Both files declare the same link; neither names the other fiber by UUID.',
                     fontsize=12, color=pal['ink2'], va='top')
        fig.text(0.97, 0.02, 'PHerc0332 fiber annotations: Vesuvius Challenge open data, CC BY-NC 4.0', fontsize=8.5,
                 color=pal['ink2'], ha='right')
        frames.append(render(fig)); plt.close(fig)
    frames_to_gif(frames, OUT + 'pr-upgrade.gif', 160, colors=64, keep=('#1a7f37', '#c4302b', pal['v4_link']))


def spin_store():
    pal = DARK
    allp = np.concatenate(store['PHerc0332']['lines']); c = allp.mean(0)
    frames = []
    for i in range(30):
        az = i * 12
        fig = plt.figure(figsize=(9.6, 5.4), dpi=100, facecolor=pal['bg'])
        ax = fig.add_axes([0.01, 0.03, 0.98, 0.8]); ax.set_facecolor(pal['bg'])
        draw_store(ax, 'PHerc0332', pal, az, 8, lw=0.75, center=c)
        R = np.abs(allp - c).max(0)
        ax.set_xlim(-R[2] * 0.97, R[2] * 0.97); ax.set_ylim(-max(R[0], R[1]) * 1.05, max(R[0], R[1]) * 1.05)
        ax.set_aspect('equal'); ax.axis('off')
        fig.text(0.02, 0.95, 'PHerc0332 in an object store: 255 fibers, 208 branch links', color=pal['ink'],
                 fontsize=15, fontweight='semibold', va='top')
        fig.text(0.02, 0.885, 'read back from the store after upgrade_vc3d_fibers.py and vc_object_ingest', color=pal['ink2'],
                 fontsize=11.5, va='top')
        fig.text(0.98, 0.015, 'Vesuvius Challenge open data, CC BY-NC 4.0', color=pal['ink2'], fontsize=8.5, ha='right')
        frames.append(render(fig)); plt.close(fig)
    frames_to_gif(frames, OUT + 'pr-spin-store-0332.gif', 120, colors=48, keep=(INGESTED, pal['ink']))


if __name__ == '__main__':
    for name in sys.argv[1:] or ['store_figure', 'before_after', 'upgrade_gif', 'spin_store']:
        globals()[name]()
