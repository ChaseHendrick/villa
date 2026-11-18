"""Infer ``spiral_outward_sense`` (CW or ACW) from single z planes of a binary
surface prediction, or abstain.

For a volume without the catalog's ``z_direction_is_top_to_bottom`` and
``left_handed_coordinates``, the fit needs ``spiral_outward_sense``, which is
otherwise read off the CT by a person in VC3D. This reads it from the
prediction's sheet chains instead.

Rule, per plane at level ``LEV`` inside a disk of radius ``R_L0`` level-0
voxels around the umbilicus at the plane's z:

1. Thin the binary plane to one-pixel lines, cut every junction (remove the
   3 x 3 block around each pixel with three or more neighbors) and keep pieces
   of at least ``MINLEN`` pixels, each walked end to end.
2. Link piece ends greedily: gap at most ``GAP_F`` sheet pitches, ends
   antiparallel within 35 degrees, forward progress at least -1.5 px, lateral
   offset at most ``LAT_F`` pitches, no cycles.
3. A chain that winds at least one full turn around the center votes by the
   median of r(theta + 2 pi) - r(theta): positive is CW (radius grows with
   atan2(y, x), x the array's last axis, y the middle one), negative ACW,
   when the median's sign holds on at least ``MIN_CONS`` of the overlap.
4. Votes pool over planes; the call is the majority when there are at least
   ``min_votes`` votes and the majority's share is at least ``min_share``,
   else ``abstain``.

``symmetric`` also tracks each plane mirrored in x and adds its votes, flipped,
so pixel-order effects of thinning and walking cancel and a mirrored volume
gives exactly the opposite call.

Usage (from spiral-fitting/)::

    python -m m7_winding_constraints.outward_sense --prediction SURFACE.zarr --level 1 \\
        --umbilicus umbilicus.json --pitch 14 --z-range 2000,16000 --out sense.json

Needs only numpy and zarr (the thinning, labelling and neighbor search are
plain numpy here).
"""

import argparse
import json
import math
import sys
import time

import numpy as np

from .surface import open_volume, pairs_within

LEV = 1
R_L0 = 1500.0
MINLEN = 8
GAP_F = 1.0
LAT_F = 0.25
COS_MIN = math.cos(math.radians(35))
MIN_CONS = 0.6
SAMPLES_PER_TURN = 64
MIN_TURNS = 1.0
MIN_VOTES = 5
MIN_SHARE = 0.8

_N8 = ((-1, 0), (-1, 1), (0, 1), (1, 1), (1, 0), (1, -1), (0, -1), (-1, -1))
_WALK = ((0, 1), (1, 0), (0, -1), (-1, 0), (1, 1), (1, -1), (-1, 1), (-1, -1))


# ---------------------------------------------------------------- umbilicus

def load_umbilicus(path):
    """Sorted (z, y, x) control points from a JSON umbilicus (a list of
    {z, y, x}, or {"control_points": [...]}) or a text file of z, y, x rows."""
    txt = open(path).read().strip()
    if txt[:1] in '[{':
        d = json.loads(txt)
        if isinstance(d, dict):
            d = d.get('control_points', d.get('points'))
        pts = [(float(e['z']), float(e['y']), float(e['x'])) if isinstance(e, dict)
               else tuple(map(float, e[:3])) for e in d]
    else:
        pts = [tuple(float(v) for v in line.split(',') if v.strip())
               for line in txt.splitlines()
               if line.strip() and not line.strip().startswith('#')]
    return sorted(pts)


def center_at(pts, z):
    """(x, y) of the umbilicus at z, linear between control points, clamped
    at the ends."""
    zs = np.array([p[0] for p in pts])
    return (float(np.interp(z, zs, [p[2] for p in pts])),
            float(np.interp(z, zs, [p[1] for p in pts])))


# ---------------------------------------------------------------- image helpers

def _shift(a, dy, dx, fill=0):
    out = np.full_like(a, fill)
    H, W = a.shape
    out[max(0, dy):H + min(0, dy), max(0, dx):W + min(0, dx)] = \
        a[max(0, -dy):H + min(0, -dy), max(0, -dx):W + min(0, -dx)]
    return out


def neighbor_count(b):
    """Number of set 8-neighbors of every pixel."""
    b = b.astype(np.int32)
    return sum(_shift(b, dy, dx) for dy, dx in _N8)


def dilate3(b):
    out = b.copy()
    for dy, dx in _N8:
        out |= _shift(b, dy, dx, False)
    return out


def thin(b):
    """Zhang-Suen thinning, then removal of staircase pixels whose set
    neighbors form one 8-connected group (so a thin line has no pixel with
    three neighbors except at a true junction)."""
    img = np.pad(b.astype(bool), 1)
    while True:
        changed = False
        for step in (0, 1):
            p = [_shift(img, -dy, -dx, False) for dy, dx in _N8]   # P2..P9
            nb = sum(q.astype(np.int32) for q in p)
            seq = p + [p[0]]
            trans = sum((~seq[i] & seq[i + 1]).astype(np.int32) for i in range(8))
            P2, P4, P6, P8 = p[0], p[2], p[4], p[6]
            if step == 0:
                c = ~(P2 & P4 & P6) & ~(P4 & P6 & P8)
            else:
                c = ~(P2 & P4 & P8) & ~(P2 & P6 & P8)
            rm = img & (nb >= 2) & (nb <= 6) & (trans == 1) & c
            if rm.any():
                img &= ~rm
                changed = True
        if not changed:
            break
    nb = neighbor_count(img)
    for y, x in zip(*np.nonzero(img & (nb >= 3))):
        ring = [bool(img[y + dy, x + dx]) for dy, dx in _N8]
        if sum(ring) < 2 or not _one_group(ring):
            continue
        img[y, x] = False
    return img[1:-1, 1:-1]


def _one_group(ring):
    """True when the set positions of a 3 x 3 ring are one 8-connected group."""
    on = [i for i in range(8) if ring[i]]
    pos = [_N8[i] for i in on]
    seen, todo = {0}, [0]
    while todo:
        a = todo.pop()
        for b in range(len(pos)):
            if b not in seen and max(abs(pos[a][0] - pos[b][0]), abs(pos[a][1] - pos[b][1])) == 1:
                seen.add(b)
                todo.append(b)
    return len(seen) == len(pos)


def label8(b):
    """8-connected component labels (0 background), by union-find."""
    ys, xs = np.nonzero(b)
    idx = -np.ones(b.shape, np.int64)
    idx[ys, xs] = np.arange(len(ys))
    parent = np.arange(len(ys))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for dy, dx in ((0, 1), (1, -1), (1, 0), (1, 1)):
        j = _shift(idx, -dy, -dx, -1)
        a = idx[b & (j >= 0)]
        c = j[b & (j >= 0)]
        for u, v in zip(a.tolist(), c.tolist()):
            ru, rv = find(u), find(v)
            if ru != rv:
                parent[max(ru, rv)] = min(ru, rv)
    roots = np.array([find(i) for i in range(len(ys))], np.int64)
    _, lab = np.unique(roots, return_inverse=True)
    out = np.zeros(b.shape, np.int64)
    out[ys, xs] = lab + 1
    return out, int(lab.max() + 1) if len(lab) else 0


# ---------------------------------------------------------------- tracking

def pieces(b):
    """Junction-free skeleton pieces of at least MINLEN pixels, each an
    ordered (n, 2) array of x, y."""
    sk = thin(b)
    nb = neighbor_count(sk)
    sk = sk & ~dilate3(sk & (nb >= 3))
    lab, n = label8(sk)
    nb = neighbor_count(sk) * sk
    out = []
    ys_all, xs_all = np.nonzero(lab)
    order = np.argsort(lab[ys_all, xs_all], kind='stable')
    ys_all, xs_all = ys_all[order], xs_all[order]
    cuts = np.searchsorted(lab[ys_all, xs_all], np.arange(1, n + 2))
    for i in range(n):
        ys, xs = ys_all[cuts[i]:cuts[i + 1]], xs_all[cuts[i]:cuts[i + 1]]
        if len(ys) < MINLEN:
            continue
        where = {(int(y), int(x)): k for k, (y, x) in enumerate(zip(ys, xs))}
        ends = np.nonzero(nb[ys, xs] == 1)[0]
        cur = int(ends[0]) if len(ends) else 0
        seq, seen = [cur], {cur}
        while True:
            y, x = int(ys[cur]), int(xs[cur])
            nxt = None
            for dy, dx in _WALK:
                k = where.get((y + dy, x + dx))
                if k is not None and k not in seen:
                    nxt = k
                    break
            if nxt is None:
                break
            seq.append(nxt)
            seen.add(nxt)
            cur = nxt
        if len(seq) >= MINLEN:
            out.append(np.stack([xs[seq], ys[seq]], 1).astype(float))
    return out


def link(ps, gap, lat_max):
    """Greedy end linking of pieces into chains (see the module rule)."""
    if not ps:
        return []
    ends = []
    for i, p in enumerate(ps):
        k = min(6, len(p) - 1)
        for e, (pos, d) in enumerate(((p[0], p[0] - p[k]), (p[-1], p[-1] - p[-1 - k]))):
            ends.append((i, e, pos, d / (np.linalg.norm(d) + 1e-9)))
    P = np.array([e[2] for e in ends])
    D = np.array([e[3] for e in ends])
    ia, ib = pairs_within(P, P, gap)
    keep = ia < ib
    cands = []
    for a, b in zip(ia[keep].tolist(), ib[keep].tolist()):
        if ends[a][0] == ends[b][0]:
            continue
        da, db = D[a], D[b]
        if -(da @ db) < COS_MIN:
            continue
        v = P[b] - P[a]
        if np.linalg.norm(v) > 1e-9:
            if v @ da < -1.5 or -v @ db < -1.5:
                continue
            lat = max(abs(da[0] * v[1] - da[1] * v[0]), abs(db[0] * v[1] - db[1] * v[0]))
        else:
            lat = 0.0
        if lat <= lat_max:
            cands.append((float(np.linalg.norm(v) + 3 * lat), a, b))
    cands.sort()
    parent = list(range(len(ps)))
    used, nxt = set(), {}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for _, a, b in cands:
        if a in used or b in used:
            continue
        ra, rb = find(ends[a][0]), find(ends[b][0])
        if ra == rb:
            continue
        parent[ra] = rb
        used |= {a, b}
        ea, eb = ends[a][:2], ends[b][:2]
        nxt[ea] = eb
        nxt[eb] = ea
    chains, done = [], set()
    for i in range(len(ps)):
        if i in done:
            continue
        cur, ce = i, 0
        while (cur, ce) in nxt:
            j, je = nxt[(cur, ce)]
            cur, ce = j, 1 - je
        seq, e_in = [], ce
        while True:
            done.add(cur)
            seq.append(ps[cur] if e_in == 0 else ps[cur][::-1])
            if (cur, 1 - e_in) not in nxt:
                break
            cur, e_in = nxt[(cur, 1 - e_in)]
        chains.append(np.concatenate(seq))
    return chains


def turn_gain(xy, cx, cy):
    """(turns, median radius gain per turn, sign consistency) of a curve; gain
    is None for a curve under MIN_TURNS turns."""
    r = np.hypot(xy[:, 0] - cx, xy[:, 1] - cy)
    th = np.unwrap(np.arctan2(xy[:, 1] - cy, xy[:, 0] - cx))
    if th[-1] < th[0]:
        th, r = th[::-1], r[::-1]
    run = np.maximum.accumulate(th)
    keep = np.r_[True, np.diff(run) > 0]
    th, r = run[keep], r[keep]
    turns = float((th[-1] - th[0]) / (2 * math.pi)) if len(th) > 1 else 0.0
    if turns < MIN_TURNS:
        return turns, None, None
    span = th[-1] - th[0] - 2 * math.pi
    n = max(2, int(SAMPLES_PER_TURN * max(span, 1e-9) / (2 * math.pi)) + 1)
    t = np.linspace(th[0], th[-1] - 2 * math.pi, n)
    dr = np.interp(t + 2 * math.pi, th, r) - np.interp(t, th, r)
    med = float(np.median(dr))
    return turns, med, float(np.mean(np.sign(dr) == np.sign(med)))


def plane_chains(img, cx, cy, pitch_px, r_px):
    """Wound chains of one binary plane around (cx, cy), with their votes."""
    H, W = img.shape
    yy, xx = np.ogrid[:H, :W]
    b = (img > 0) & ((xx - cx) ** 2 + (yy - cy) ** 2 < r_px ** 2)
    out = []
    for c in link(pieces(b), GAP_F * pitch_px, LAT_F * pitch_px):
        th = np.unwrap(np.arctan2(c[:, 1] - cy, c[:, 0] - cx))
        if abs(th[-1] - th[0]) < 2 * math.pi * 0.95 and np.ptp(th) < 2 * math.pi:
            continue
        turns, g, cons = turn_gain(c, cx, cy)
        if g is None:
            continue
        vote = ('CW' if g > 0 else 'ACW') if (cons >= MIN_CONS and g != 0) else None
        out.append({'turns': round(turns, 3), 'gain_px': round(g, 2),
                    'consistency': round(cons, 3), 'vote': vote})
    return out


def plane_votes(img, cx, cy, pitch_px, r_px, symmetric=True):
    """(CW, ACW) votes of one plane; with symmetric, the mirrored plane's
    votes are added flipped."""
    v = [q['vote'] for q in plane_chains(img, cx, cy, pitch_px, r_px)]
    cw, acw = v.count('CW'), v.count('ACW')
    if symmetric:
        m = [q['vote'] for q in plane_chains(img[:, ::-1], img.shape[1] - 1 - cx, cy, pitch_px, r_px)]
        cw, acw = cw + m.count('ACW'), acw + m.count('CW')
    return cw, acw


def decide(cw, acw, min_votes=MIN_VOTES, min_share=MIN_SHARE):
    n = cw + acw
    if n < min_votes or max(cw, acw) / n < min_share:
        return 'abstain'
    return 'CW' if cw > acw else 'ACW'


# ---------------------------------------------------------------- volume

def read_plane(arr, umb, z, level=LEV, r_l0=R_L0):
    """The level-``level`` plane at level-0 z, cropped to the core disk's box;
    returns (plane, cx, cy) in plane pixels."""
    s = 2 ** level
    cx, cy = center_at(umb, z)
    H, W = arr.shape[1], arr.shape[2]
    x0, x1 = max(0, int((cx - r_l0) / s)), min(W, int((cx + r_l0) / s) + 1)
    y0, y1 = max(0, int((cy - r_l0) / s)), min(H, int((cy + r_l0) / s) + 1)
    img = np.asarray(arr[int(z) // s, y0:y1, x0:x1])
    return img, cx / s - x0, cy / s - y0


def infer(arr, umb, z0, z1, pitch_l0, planes=104, level=LEV, symmetric=True, mirror=False,
          min_votes=MIN_VOTES, min_share=MIN_SHARE, say=None):
    """The call over ``planes`` planes evenly inside (z0, z1), with per-plane
    vote counts. ``mirror`` flips every plane in x first (the call must flip)."""
    s = 2 ** level
    rows, t0 = [], time.time()
    for k, z in enumerate(int(v) for v in np.linspace(z0, z1, planes + 2)[1:-1]):
        img, cx, cy = read_plane(arr, umb, z, level)
        if mirror:
            img, cx = img[:, ::-1], img.shape[1] - 1 - cx
        cw, acw = plane_votes(img, cx, cy, pitch_l0 / s, R_L0 / s, symmetric)
        rows.append({'z': z, 'CW': cw, 'ACW': acw})
        if say and cw + acw:
            say(f'{k + 1}/{planes} z={z} CW={cw} ACW={acw} {time.time() - t0:.0f}s')
    cw, acw = sum(r['CW'] for r in rows), sum(r['ACW'] for r in rows)
    return {'call': decide(cw, acw, min_votes, min_share), 'CW': cw, 'ACW': acw,
            'planes': len(rows), 'planes_voting': sum(1 for r in rows if r['CW'] + r['ACW']),
            'rule': {'level': level, 'r_l0': R_L0, 'minlen': MINLEN, 'gap_pitch': GAP_F,
                     'lat_pitch': LAT_F, 'min_consistency': MIN_CONS, 'min_turns': MIN_TURNS,
                     'min_votes': min_votes, 'min_share': min_share, 'pitch_l0': pitch_l0,
                     'symmetric': symmetric, 'mirror': mirror},
            'rows': rows}


# ---------------------------------------------------------------- synthetic

def synthetic(sense, rings=False, n=700, pitch=9.0, seed=0, turns=30):
    """A binary plane of an Archimedean spiral (sense 'CW': radius grows with
    atan2(y, x)) squashed 0.6 in y, rotated, with breaks; or closed rings.
    Returns (plane, cx, cy, pitch in plane pixels)."""
    rng = np.random.default_rng(seed)
    img = np.zeros((n, n), bool)
    c = n / 2
    th = np.linspace(0, 2 * math.pi * turns, 400000)
    r = 40 + pitch * (th / (2 * math.pi)) if not rings else 40 + pitch * np.floor(th / (2 * math.pi))
    sg = 1 if sense == 'CW' else -1
    x, y = r * np.cos(sg * th), r * np.sin(sg * th) * 0.6
    a = 0.4
    x, y = x * math.cos(a) - y * math.sin(a), x * math.sin(a) + y * math.cos(a)
    keep = np.ones(len(th), bool)
    for k in rng.integers(0, len(th), 60):
        keep[k:k + 300] = False
    xi, yi = np.round(c + x[keep]).astype(int), np.round(c + y[keep]).astype(int)
    ok = (xi >= 0) & (xi < n) & (yi >= 0) & (yi < n)
    img[yi[ok], xi[ok]] = True
    return dilate3(img), c, c, pitch * 0.6


# ---------------------------------------------------------------- command line

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--prediction', required=True, help='binary surface prediction zarr (group or array)')
    ap.add_argument('--level', type=int, default=LEV, help='pyramid level read (default %(default)s)')
    ap.add_argument('--umbilicus', required=True)
    ap.add_argument('--pitch', type=float, required=True, help='sheet pitch, level-0 voxels')
    ap.add_argument('--z-range', required=True, help='z0,z1 in level-0 voxels')
    ap.add_argument('--planes', type=int, default=104)
    ap.add_argument('--mirror', action='store_true', help='flip every plane in x (control: the call must flip)')
    ap.add_argument('--no-symmetric', action='store_true')
    ap.add_argument('--min-votes', type=int, default=MIN_VOTES)
    ap.add_argument('--min-share', type=float, default=MIN_SHARE)
    ap.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    z0, z1 = (float(v) for v in a.z_range.split(','))
    arr = open_volume(a.prediction, a.level)
    res = infer(arr, load_umbilicus(a.umbilicus), z0, z1, a.pitch, a.planes, a.level,
                not a.no_symmetric, a.mirror, a.min_votes, a.min_share,
                say=lambda m: print(m, file=sys.stderr, flush=True))
    with open(a.out, 'w') as fh:
        json.dump(res, fh, indent=1)
    print(json.dumps({k: res[k] for k in ('call', 'CW', 'ACW', 'planes', 'planes_voting')}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
