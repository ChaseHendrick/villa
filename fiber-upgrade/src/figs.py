"""Figures for the villa issue on fiber ingest (version 4 branches without target_uuid)."""
import json, pickle, sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager as fm
from matplotlib.collections import LineCollection
from matplotlib.patches import FancyBboxPatch

for f in fm.findSystemFonts():
    if 'Inter' in f or 'DejaVuSansMono' in f:
        fm.fontManager.addfont(f)
plt.rcParams.update({'font.family': 'Inter', 'font.size': 13})
MONO = 'DejaVu Sans Mono'

LIGHT = dict(bg='#fcfcfb', ink='#0b0b0b', ink2='#52514e', grid='#e4e3df',
             v3='#2a78d6', v4_link='#eb6834', v4_nolink='#1baf7a')
DARK = dict(bg='#1a1a19', ink='#f0efec', ink2='#c3c2b7', grid='#3a3a37',
            v3='#3987e5', v4_link='#d95926', v4_nolink='#199e70')
LABEL = {'v4_link': 'version 4, links named by file only',
         'v4_nolink': 'version 4, no links',
         'v3': 'version 3 (links by file, supported)'}

data = pickle.load(open('fig/fibers.pkl', 'rb'))
res = json.load(open('fig/ingest-results.json'))
OUT = 'fig/out/'


def rot(points, az, el):
    """Orthographic view: azimuth about the scroll axis (z), then elevation."""
    a, e = np.radians(az), np.radians(el)
    x, y, z = points[:, 0], points[:, 1], points[:, 2]
    u = x * np.cos(a) - y * np.sin(a)
    w = x * np.sin(a) + y * np.cos(a)
    v = z * np.cos(e) + w * np.sin(e)
    depth = w * np.cos(e) - z * np.sin(e)
    return np.stack([v, -u], 1), depth


def draw_fibers(ax, files, pal, az, el, lw=0.9, center=None, pts=True):
    allp = np.concatenate([f['line'] for f in files])
    c = allp.mean(0) if center is None else center
    order = ['v3', 'v4_nolink', 'v4_link']
    for kind in order:
        segs = []
        for f in files:
            if f['kind'] != kind:
                continue
            p, _ = rot(f['line'] - c, az, el)
            segs.append(p)
        alpha = 0.95 if kind == 'v4_link' else 0.75
        ax.add_collection(LineCollection(segs, colors=pal[kind], linewidths=lw, alpha=alpha,
                                         capstyle='round', joinstyle='round'))
    if pts:
        bp = [b for f in files if f['kind'] == 'v4_link' for b in f['branch_pts']]
        if bp:
            p, _ = rot(np.asarray(bp) - c, az, el)
            ax.scatter(p[:, 0], p[:, 1], s=16, color=pal['ink'], edgecolors=pal['bg'], linewidths=0.8, zorder=5)
    allr, _ = rot(allp - c, az, el)
    return allr


def hero():
    pal = DARK
    fig = plt.figure(figsize=(16, 10.5), dpi=110, facecolor=pal['bg'])
    boxes = {'PHerc0139': [0.02, 0.53, 0.68, 0.36], 'PHerc0332': [0.02, 0.1, 0.68, 0.36]}
    for scroll, box in boxes.items():
        ax = fig.add_axes(box); ax.set_facecolor(pal['bg'])
        files = data[scroll]
        r = draw_fibers(ax, files, pal, 35, 10)
        lo, hi = np.percentile(r, 0.3, 0), np.percentile(r, 99.7, 0)
        pad = (hi - lo) * 0.03
        ax.set_xlim(lo[0] - pad[0], hi[0] + pad[0]); ax.set_ylim(lo[1] - pad[1], hi[1] + pad[1])
        ax.set_aspect('equal', anchor='W'); ax.axis('off')
        n = {k: sum(f['kind'] == k for f in files) for k in LABEL}
        nb = sum(f['n_branch'] for f in files if f['version'] == 4)
        tx = 0.72
        ty = box[1] + box[3]
        fig.text(tx, ty, scroll, color=pal['ink'], fontsize=20, fontweight='semibold', va='top')
        fig.text(tx, ty - 0.045, f'{len(files)} published fiber files', color=pal['ink2'], fontsize=13.5, va='top')
        rows = [(pal['v4_link'], f'{n["v4_link"]} version 4 files with {nb} links,'),
                (pal['v4_link'], '    none with target_uuid'),
                (pal['v4_nolink'], f'{n["v4_nolink"]} version 4 files without links')]
        if n['v3']:
            rows.append((pal['v3'], f'{n["v3"]} version 3 files'))
        y = ty - 0.1
        for col, t in rows:
            if not t.startswith('    '):
                fig.text(tx, y, '\u25cf', color=col, fontsize=14, va='top')
            fig.text(tx + 0.02, y, t.strip(), color=pal['ink'], fontsize=13.5, va='top')
            y -= 0.035
        y -= 0.015
        fig.text(tx, y, 'ingested as published:', color=pal['ink2'], fontsize=13.5, va='top')
        fig.text(tx + 0.155, y + 0.004, f'{res[scroll[-4:] + " published"]["fibers"]} fibers', color=pal['ink'],
                 fontsize=16, fontweight='bold', va='top')
    fig.text(0.02, 0.965, 'The published fiber annotations, colored by what store ingest makes of them', color=pal['ink'],
             fontsize=19, fontweight='semibold', va='top')
    fig.text(0.02, 0.925, "Each fiber's saved line_points, scroll axis left to right, seen at an angle. White dots: control points "
             'that carry a branch link.', color=pal['ink2'], fontsize=12.5, va='top')
    fig.text(0.98, 0.02, 'Fiber annotations: Vesuvius Challenge open data, CC BY-NC 4.0', color=pal['ink2'],
             fontsize=10.5, ha='right')
    fig.savefig(OUT + 'ingest-fibers.png', facecolor=pal['bg'])
    plt.close(fig)


def counts_chart():
    pal = LIGHT
    fig, ax = plt.subplots(figsize=(14, 5.2), dpi=110, facecolor=pal['bg'])
    ax.set_facecolor(pal['bg'])
    scrolls = ['PHerc0139', 'PHerc0332']
    kinds = ['v4_link', 'v4_nolink', 'v3']
    for yi, s in enumerate(scrolls):
        left = 0
        for k in kinds:
            n = sum(f['kind'] == k for f in data[s])
            if not n:
                continue
            ax.barh(yi, n - 2, left=left + 1, height=0.56, color=pal[k], edgecolor='none')
            if n >= 30:
                ax.text(left + n / 2, yi, str(n), ha='center', va='center', color='white' if k != 'v4_nolink' else pal['ink'],
                        fontsize=15, fontweight='bold')
            else:
                ax.text(left + n / 2, yi - 0.36, str(n), ha='center', va='bottom', color=pal['ink'], fontsize=13, fontweight='bold')
            left += n
        ax.text(left + 8, yi, f'{left} files', va='center', color=pal['ink2'], fontsize=13)
    ax.set_yticks(range(2), scrolls, fontsize=15, color=pal['ink'])
    ax.invert_yaxis()
    ax.set_xlim(0, 700)
    ax.xaxis.grid(True, color=pal['grid'], lw=0.8); ax.set_axisbelow(True)
    for sp in ax.spines.values():
        sp.set_visible(False)
    ax.tick_params(axis='x', colors=pal['ink2'], labelsize=12, length=0)
    ax.tick_params(axis='y', length=0)
    ax.set_xlabel('published fiber files', color=pal['ink2'], fontsize=12.5)
    handles = [plt.Rectangle((0, 0), 1, 1, color=pal[k]) for k in kinds]
    ax.legend(handles, [LABEL[k] for k in kinds], loc='lower center', bbox_to_anchor=(0.5, 1.02), ncol=3,
              frameon=False, fontsize=12.5, labelcolor=pal['ink'], handlelength=1.2)
    fig.suptitle('Most published fiber files are version 4 with branch links that name the other fiber only by file',
                 x=0.012, ha='left', y=0.975, fontsize=16, fontweight='semibold', color=pal['ink'])
    fig.subplots_adjust(left=0.1, right=0.98, top=0.76, bottom=0.14)
    fig.savefig(OUT + 'ingest-file-counts.png', facecolor=pal['bg'])
    plt.close(fig)


def outcome_chart():
    pal = LIGHT
    fig, axes = plt.subplots(1, 2, figsize=(14, 5.6), dpi=110, facecolor=pal['bg'])
    for ax, metric, title in zip(axes, ['fibers', 'edges_imported'], ['fibers in the store', 'branch edges in the store']):
        ax.set_facecolor(pal['bg'])
        x = np.arange(2)
        for i, s in enumerate(['0139', '0332']):
            a = res[s + ' published'][metric]
            b = res[s + ' as v3'][metric]
            ax.bar(i - 0.2, max(a, 0.001), 0.36, color=pal['v4_link'])
            ax.bar(i + 0.2, b, 0.36, color=pal['v3'])
            ax.text(i - 0.2, a + 12, str(a), ha='center', va='bottom', fontsize=14, fontweight='bold', color=pal['ink'])
            ax.text(i + 0.2, b + 12, str(b), ha='center', va='bottom', fontsize=14, fontweight='bold', color=pal['ink'])
        ax.set_xticks(x, ['PHerc0139', 'PHerc0332'], fontsize=14, color=pal['ink'])
        ax.set_ylim(0, 720)
        ax.yaxis.grid(True, color=pal['grid'], lw=0.8); ax.set_axisbelow(True)
        for sp in ax.spines.values():
            sp.set_visible(False)
        ax.tick_params(axis='y', colors=pal['ink2'], labelsize=12, length=0)
        ax.tick_params(axis='x', length=0)
        ax.set_title(title, loc='left', fontsize=14.5, color=pal['ink'], fontweight='semibold')
    handles = [plt.Rectangle((0, 0), 1, 1, color=pal['v4_link']), plt.Rectangle((0, 0), 1, 1, color=pal['v3'])]
    fig.legend(handles, ['as published (version 4)', 'same files relabeled as version 3'], loc='upper right',
               bbox_to_anchor=(0.985, 0.985), ncol=2, frameon=False, fontsize=12.5, labelcolor=pal['ink'])
    fig.suptitle('vc_object_ingest on villa main 39a8cf3', x=0.012, ha='left', y=0.975, fontsize=16,
                 fontweight='semibold', color=pal['ink'])
    fig.text(0.012, 0.02, "As published, each folder stops at the first version 4 link: key 'target_uuid' not found. "
             'Only the version field was changed for the relabeled run.', fontsize=11.5, color=pal['ink2'])
    fig.subplots_adjust(left=0.05, right=0.985, top=0.82, bottom=0.14, wspace=0.18)
    fig.savefig(OUT + 'ingest-outcome.png', facecolor=pal['bg'])
    plt.close(fig)


def card(ax, x, y, w, h, title, rows, pal, accent):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle='round,pad=0,rounding_size=0.018', fc='white',
                                ec=pal['grid'], lw=1.4))
    ax.add_patch(FancyBboxPatch((x, y + h - 0.006), w, 0.006, boxstyle='square,pad=0', fc=accent, ec='none'))
    ax.text(x + 0.02, y + h - 0.05, title, fontsize=14, fontweight='semibold', color=pal['ink'], va='top')
    yy = y + h - 0.13
    for text, style in rows:
        color = {'ok': pal['ink'], 'dim': pal['ink2'], 'bad': '#c4302b', 'good': '#1a7f37'}[style]
        weight = 'bold' if style in ('bad', 'good') else 'normal'
        ax.text(x + 0.025, yy, text, family=MONO, fontsize=12, color=color, va='top', fontweight=weight)
        yy -= 0.052


def format_figure():
    pal = LIGHT
    fig = plt.figure(figsize=(16, 8.4), dpi=110, facecolor=pal['bg'])
    ax = fig.add_axes([0, 0, 1, 1]); ax.axis('off'); ax.set_xlim(0, 1); ax.set_ylim(0, 1)
    ax.text(0.03, 0.95, 'One branch link, as VC3D saved it and as ingest reads it', fontsize=18,
            fontweight='semibold', color=pal['ink'], va='top')
    ex = next(f for f in data['PHerc0332'] if f['name'] == 'lt_20261016T073323561_000068.json')
    b = ex['branches'][0]
    rows = [('"version": 4,', 'ok'), ('"branches": [{', 'ok'),
            (f'  "branch_file": "{b["branch_file"]}",', 'good'),
            (f'  "branch_fiber_id": {b["branch_fiber_id"]},', 'dim'),
            (f'  "control_point_index": {b["control_point_index"]},', 'dim'),
            (f'  "branch_control_point_index": {b["branch_control_point_index"]},', 'dim'),
            ('  "control_point_direction": [...],', 'dim'),
            ('  "branch_control_point_direction": [...],', 'dim'),
            ('  "pending": true', 'dim'), ('}]', 'ok')]
    card(ax, 0.03, 0.17, 0.455, 0.69, 'PHerc0332/' + ex['name'], rows, pal, pal['v4_link'])
    rows2 = [('const bool byFile =', 'ok'), ('    metadata.value("version", 0) < 4;', 'bad'), ('', 'ok'),
             ('if (byFile)  // versions 1 and 3', 'ok'), ('    branch["target_uuid"] =', 'dim'),
             ('        uuid of branch_file;', 'dim'), ('', 'ok'),
             ('branch.at("target_uuid")  // version 4:', 'ok'), ('    must already be in the file', 'bad')]
    card(ax, 0.515, 0.17, 0.455, 0.69, 'vc_object_store/src/formats/FiberInput.cpp (main 39a8cf3)', rows2, pal, pal['v3'])
    ax.text(0.03, 0.115, 'VC3D before #2316 wrote kFiberFormatVersion = 4 (LineAnnotationFiberSegments.hpp) with branch_file '
            'links (LineAnnotationController.cpp) and never target_uuid.', fontsize=12.5, color=pal['ink2'])
    ax.text(0.03, 0.07, 'Ingest treats only versions below 4 as linked by file, so every link VC3D saved throws '
            "key 'target_uuid' not found and the whole folder is refused.", fontsize=12.5, color=pal['ink2'])
    fig.savefig(OUT + 'ingest-format.png', facecolor=pal['bg'])
    plt.close(fig)


def frames_to_gif(frames, path, ms, colors=96, keep=()):
    """Quantize every frame to one shared palette; `keep` colors get a swatch so rare legend dots survive."""
    from PIL import Image
    src = frames[0]
    if keep:
        sw = np.concatenate([np.full((40, src.shape[1], 3), [int(k[i:i + 2], 16) for i in (1, 3, 5)], np.uint8)
                             for k in keep])
        src = np.concatenate([src, sw])
    pal_img = Image.fromarray(src).quantize(colors=colors, method=Image.Quantize.MEDIANCUT)
    imgs = [Image.fromarray(f).quantize(palette=pal_img, dither=Image.Dither.NONE) for f in frames]
    imgs[0].save(path, save_all=True, append_images=imgs[1:], duration=ms, loop=0, optimize=True, disposal=2)


def render(fig):
    fig.canvas.draw()
    return np.asarray(fig.canvas.buffer_rgba())[:, :, :3].copy()


def spin_gif():
    pal = DARK
    files = data['PHerc0332']
    allp = np.concatenate([f['line'] for f in files]); c = allp.mean(0)
    frames = []
    nfr = 30
    for i in range(nfr):
        az = i * 360 / nfr
        fig = plt.figure(figsize=(9.6, 5.4), dpi=100, facecolor=pal['bg'])
        ax = fig.add_axes([0.01, 0.03, 0.98, 0.8]); ax.set_facecolor(pal['bg'])
        draw_fibers(ax, files, pal, az, 8, lw=0.75, center=c)
        R = np.abs(allp - c).max(0)
        ax.set_xlim(-R[2] * 0.97, R[2] * 0.97); ax.set_ylim(-max(R[0], R[1]) * 1.05, max(R[0], R[1]) * 1.05)
        ax.set_aspect('equal'); ax.axis('off')
        fig.text(0.02, 0.95, 'PHerc0332: all 255 published fibers are version 4', color=pal['ink'], fontsize=15,
                 fontweight='semibold', va='top')
        fig.text(0.02, 0.885, '●', color=pal['v4_link'], fontsize=12, va='top')
        fig.text(0.04, 0.885, '202 with 416 links by file name only', color=pal['ink2'], fontsize=11.5, va='top')
        fig.text(0.42, 0.885, '●', color=pal['v4_nolink'], fontsize=12, va='top')
        fig.text(0.44, 0.885, '53 without links', color=pal['ink2'], fontsize=11.5, va='top')
        fig.text(0.62, 0.885, 'ingested as published: 0', color=pal['ink'], fontsize=11.5, fontweight='bold', va='top')
        fig.text(0.98, 0.015, 'Vesuvius Challenge open data, CC BY-NC 4.0', color=pal['ink2'], fontsize=8.5, ha='right')
        frames.append(render(fig)); plt.close(fig)
    frames_to_gif(frames, OUT + 'ingest-spin-0332.gif', 120, colors=48, keep=(pal['v4_nolink'], pal['v4_link'], pal['ink']))


def link_gif():
    pal = DARK
    files = {f['name']: f for f in data['PHerc0332']}
    a = files['lt_20261016T073323561_000068.json']
    b_entry = a['branches'][0]
    b = files[b_entry['branch_file']]
    full = {}
    for f in (a, b):
        j = json.load(open('data/0332_fibers/' + f['name']))
        full[f['name']] = np.asarray(j['line_points'], float)
    pa = np.asarray(b_entry['control_point_position']); pb = np.asarray(b_entry['branch_control_point_position'])
    c = (pa + pb) / 2
    R = 200
    frames = []
    nfr = 36
    for i in range(nfr):
        az = i * 360 / nfr
        fig = plt.figure(figsize=(9.6, 5.4), dpi=100, facecolor=pal['bg'])
        ax = fig.add_axes([0.0, 0.02, 0.56, 0.86]); ax.set_facecolor(pal['bg'])
        for name, col in ((a['name'], pal['v4_link']), (b['name'], '#e8c6b5')):
            p = full[name]
            near = np.linalg.norm(p - c, axis=1) < R * 1.6
            q, _ = rot(p[near] - c, az, 15)
            ax.plot(q[:, 0], q[:, 1], color=col, lw=2.2, solid_capstyle='round')
        for pt in (pa, pb):
            q, _ = rot(pt[None] - c, az, 15)
            ax.scatter(q[:, 0], q[:, 1], s=70, color=pal['ink'], edgecolors=pal['bg'], linewidths=1.5, zorder=5)
        qa, _ = rot(pa[None] - c, az, 15); qb, _ = rot(pb[None] - c, az, 15)
        ax.plot([qa[0, 0], qb[0, 0]], [qa[0, 1], qb[0, 1]], color=pal['ink'], lw=1.2, ls=(0, (3, 2)), zorder=4)
        ax.set_xlim(-R, R); ax.set_ylim(-R, R); ax.set_aspect('equal'); ax.axis('off')
        fig.text(0.02, 0.95, 'One branch link in PHerc0332', color=pal['ink'], fontsize=15, fontweight='semibold', va='top')
        x0, y = 0.58, 0.8
        fig.text(x0, y, 'from', color=pal['ink2'], fontsize=11, va='top')
        fig.text(x0, y - 0.05, a['name'], color=pal['v4_link'], fontsize=11.5, family=MONO, va='top')
        fig.text(x0, y - 0.11, 'control point 7', color=pal['ink2'], fontsize=11, va='top')
        fig.text(x0, y - 0.2, 'to', color=pal['ink2'], fontsize=11, va='top')
        fig.text(x0, y - 0.25, b['name'], color='#e8c6b5', fontsize=11.5, family=MONO, va='top')
        fig.text(x0, y - 0.31, 'control point 8', color=pal['ink2'], fontsize=11, va='top')
        fig.text(x0, y - 0.43, '"version": 4', color=pal['ink'], fontsize=11.5, family=MONO, va='top')
        fig.text(x0, y - 0.49, '"branch_file": present', color='#4ac26b', fontsize=11.5, family=MONO, va='top')
        fig.text(x0, y - 0.55, '"target_uuid": missing', color='#ff7b72', fontsize=11.5, family=MONO, va='top')
        fig.text(x0, y - 0.65, "ingest: key 'target_uuid' not found", color=pal['ink'], fontsize=11, va='top')
        fig.text(0.98, 0.015, 'Vesuvius Challenge open data, CC BY-NC 4.0', color=pal['ink2'], fontsize=8.5, ha='right')
        frames.append(render(fig)); plt.close(fig)
    frames_to_gif(frames, OUT + 'ingest-one-link.gif', 100, keep=('#4ac26b', '#ff7b72', '#e8c6b5'))


if __name__ == '__main__':
    import os
    os.makedirs(OUT, exist_ok=True)
    for name in sys.argv[1:] or ['hero', 'counts_chart', 'outcome_chart', 'format_figure']:
        globals()[name]()
