"""Give every m7 sheet piece of a z slice a winding number in one solve.

Model output from a binary surface prediction (such as m7); it is not
evidence of ink.

Idea (phase unwrapping). The label of a sheet piece is its winding number k,
with unwrapped angle Phi = theta' + 2 pi k growing outward, where
theta' = mod(hand * atan2(y - cy, x - cx), 2 pi) about the umbilicus and hand
is +1 for a CW spiral (radius grows with atan2(y, x), x the array's last axis)
and -1 for ACW. Local facts become soft constraints on integer labels:

  continuity  consecutive pieces of one skeleton path carry the same label,
              or +1 / -1 where the path crosses the start ray theta' = 0
              (L1 cost c_cont per unit of violation)
  radial      the next sheet out along the outward normal has a label at
              least one higher (cost c_hi per unit below 1), preferably exactly
              one higher (cost c_lo per unit above 1, so a sheet the
              prediction missed costs little when the rest of the slice says
              it is there)

All costs are convex piecewise linear in label differences with integer
breakpoints, so this is a minimum-cost tension problem (the dual of a
minimum-cost flow): its linear relaxation has an integral optimum, found
exactly here with scipy's HiGHS. Each connected component of the piece graph
is solved on its own offset (its first piece at 0); the ``comp`` array says
which, and labels compare only inside one component.

Usage (from spiral-fitting/)::

    python -m m7_winding_constraints.label_slices --prediction M7.zarr \\
        --umbilicus umbilicus.json --sense sense.json --z 3000 3400 --out slices/

writes ``slices/zNNNNN.npz`` per slice (the skeleton pixels with their
piece, label, component, theta' and radius, plus the piece graph) and a line
per slice in ``slices/runs.jsonl``. ``ladders.py`` turns them into winding
constraints for the fit. Lengths in PARAMS are pixels of the plane read
(level 0 of a scan near 9 um in the tests this was made for); pass
``--params`` to change them for another resolution.

Needs numpy, scipy and zarr.
"""

import argparse
import concurrent.futures
import json
import math
import sys
import time
from pathlib import Path

import numpy as np
from scipy import ndimage, sparse
from scipy.optimize import linprog
from scipy.sparse import csgraph

from .outward_sense import center_at, load_umbilicus, thin
from .surface import open_volume

PARAMS = {
    'threshold': 127,           # prediction values above this are sheet
    'block_fill': 0.9,          # 64 px tiles at least this full are chunk fill, not sheets
    'block_margin': 4,          # px removed around chunk-fill tiles (their edges are not sheets)
    'r_min': 150.0,             # pixels nearer the umbilicus are left out (theta is ill-defined there)
    'piece_len': 40,            # skeleton pixels per piece along a path
    'min_piece': 4,             # pieces shorter than this get no radial samples of their own
    'radial_every': 8,          # one radial sample per this many skeleton pixels
    'tangent_half': 5,          # path pixels each side used for the local tangent
    'max_step': 40.0,           # farthest next sheet looked for along the normal (px)
    'march_step': 0.5,
    'skel_snap': 3.0,           # a hit maps to the skeleton pixel nearest the next sheet's center within this
    'spur_len': 8,              # skeleton branches from an end to a junction shorter than this are pruned
    'spur_rounds': 2,
    'c_hi': 20.0,               # cost per unit a radial difference falls below 1
    'c_lo': 1.0,                # cost per unit a radial difference exceeds 1
    'c_cont': 10.0,             # cost per unit a continuity difference misses its shift
    'track_rays': 1440,         # track_hand: rays per turn
    'track_r_max': 3300.0,      # track_hand: farthest radius read
    'track_r_lo': 300.0,        # track_hand: start radii
    'track_r_hi': 2600.0,
    'track_max_width': 12.0,    # track_hand: thicker runs are blobs, not sheets
}


# ---------------------------------------------------------------- slice preparation

def clean_slice(s, p=PARAMS):
    """Binary sheet mask with chunk-fill tiles removed."""
    S = np.asarray(s) > p['threshold']
    t = 64
    H, W = S.shape
    h, w = H // t * t, W // t * t
    fill = S[:h, :w].reshape(h // t, t, w // t, t).mean(axis=(1, 3))
    bad = np.zeros_like(S)
    bad[:h, :w] = np.repeat(np.repeat(fill >= p['block_fill'], t, 0), t, 1)
    if p.get('block_margin', 0):
        bad = ndimage.binary_dilation(bad, iterations=int(p['block_margin']))
    return S & ~bad


_NB = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]


def skel_graph(K):
    """Sparse neighbor adjacency over skeleton pixels: (ys, xs, A, index
    image). A diagonal step is dropped when the two pixels already touch
    through a shared 4-neighbor, so staircase lines have degree 2 and only
    real junctions have degree 3 or more."""
    ys, xs = np.nonzero(K)
    n = len(ys)
    idx = -np.ones(K.shape, np.int64)
    idx[ys, xs] = np.arange(n)
    H, W = K.shape
    Kp = np.pad(K, 1)
    rows, cols = [], []
    for dy, dx in _NB:
        y2, x2 = ys + dy, xs + dx
        ok = (y2 >= 0) & (y2 < H) & (x2 >= 0) & (x2 < W)
        j = np.full(n, -1)
        j[ok] = idx[y2[ok], x2[ok]]
        m = j >= 0
        if dy and dx:
            via = Kp[ys + dy + 1, xs + 1] | Kp[ys + 1, xs + dx + 1]
            m &= ~via
        rows.append(np.arange(n)[m])
        cols.append(j[m])
    rows, cols = np.concatenate(rows), np.concatenate(cols)
    A = sparse.csr_matrix((np.ones(len(rows), np.int8), (rows, cols)), shape=(n, n))
    return ys, xs, A, idx


def prune_spurs(K, max_len, rounds=2):
    """Remove skeleton branches that run from an end pixel to a junction in
    fewer than max_len pixels."""
    K = K.copy()
    for _ in range(rounds):
        ys, xs, A, _ = skel_graph(K)
        deg = np.asarray(A.sum(axis=1)).ravel()
        indptr, indices = A.indptr, A.indices
        kill = []
        for e in np.flatnonzero(deg == 1):
            walk, prev, cur = [e], -1, e
            while len(walk) <= max_len:
                nb = [v for v in indices[indptr[cur]:indptr[cur + 1]] if v != prev]
                if not nb:
                    walk = []          # an isolated short piece: keep it (not a spur of a sheet)
                    break
                if deg[nb[0]] >= 3 or len(nb) > 1:
                    break
                prev, cur = cur, nb[0]
                walk.append(cur)
            else:
                walk = []
            if walk and len(walk) < max_len:
                kill.extend(walk)
        if not kill:
            break
        kill = np.asarray(kill)
        K[ys[kill], xs[kill]] = False
    return K


def paths(A, deg):
    """Simple paths of the skeleton with junction pixels (degree >= 3)
    removed, as index arrays in path order. Junction pixels are returned as
    one-pixel paths of their own (no continuity edges)."""
    keep = deg <= 2
    sub = A[keep][:, keep]
    kidx = np.flatnonzero(keep)
    ncomp, lab = csgraph.connected_components(sub, directed=False)
    sdeg = np.asarray(sub.sum(axis=1)).ravel()
    order = np.argsort(lab, kind='stable')
    bounds = np.searchsorted(lab[order], np.arange(ncomp + 1))
    out = []
    indptr, indices = sub.indptr, sub.indices
    for c in range(ncomp):
        mem = order[bounds[c]:bounds[c + 1]]
        if len(mem) == 1:
            out.append(kidx[mem])
            continue
        ends = mem[sdeg[mem] <= 1]
        start = ends[0] if len(ends) else mem[0]
        seq = [start]
        prev, cur = -1, start
        while True:
            nb = indices[indptr[cur]:indptr[cur + 1]]
            nxt = [v for v in nb if v != prev]
            if not nxt or (len(seq) > 1 and nxt[0] == start):
                break
            if len(seq) >= len(mem):
                break
            prev, cur = cur, nxt[0]
            seq.append(cur)
        seq = np.asarray(seq)
        if len(seq) < len(mem):     # leftovers of a messy component: singletons
            for v in np.setdiff1d(mem, seq):
                out.append(kidx[[v]])
        out.append(kidx[seq])
    for v in np.flatnonzero(~keep):
        out.append(np.asarray([v]))
    return out


def ray_shift(d):
    """Start-ray shift of a step whose theta' changes by d: +1 when it
    crosses theta' = 0 going up, -1 going down, else 0."""
    return 1 if d < -math.pi else (-1 if d > math.pi else 0)


def make_pieces(path_list, theta_p, n, p=PARAMS):
    """Cut paths into pieces of at most piece_len pixels and at every crossing
    of the start ray: (piece per skeleton pixel, pieces' pixel lists,
    continuity edges (a, b, shift))."""
    pid = -np.ones(n, np.int64)
    pieces, cont = [], []
    L = p['piece_len']
    for q in path_list:
        th = theta_p[q]
        wraps = np.flatnonzero(np.abs(np.diff(th)) > math.pi) + 1
        cuts = sorted(c for c in set(range(L, len(q), L)) | set(wraps.tolist()) if 0 < c < len(q))
        prev = None
        for s in np.split(np.arange(len(q)), cuts):
            k = len(pieces)
            pieces.append(q[s])
            pid[q[s]] = k
            if prev is not None:
                cont.append((k - 1, k, ray_shift(theta_p[q[s[0]]] - theta_p[q[prev[-1]]])))
            prev = s
    return pid, pieces, cont


def tangents(ys, xs, path_list, half):
    """Unit tangent per skeleton pixel from its path neighbors."""
    T = np.zeros((len(ys), 2))
    for q in path_list:
        if len(q) < 2:
            continue
        P = np.stack([xs[q], ys[q]], 1).astype(float)
        a = np.clip(np.arange(len(q)) - half, 0, len(q) - 1)
        b = np.clip(np.arange(len(q)) + half, 0, len(q) - 1)
        d = P[b] - P[a]
        T[q] = d / np.maximum(np.linalg.norm(d, axis=1, keepdims=True), 1e-9)
    return T


def radial_edges(S, K, ys, xs, pid, pieces, T, cx, cy, p=PARAMS):
    """From sampled skeleton pixels, march outward along the normal to the
    next sheet: rows (sample pixel, hit skeleton pixel, gap px, next run's
    width px, step px center to center)."""
    H, W = S.shape
    dist, (iy, ix) = ndimage.distance_transform_edt(~K, return_indices=True)
    idx = -np.ones(K.shape, np.int64)
    idx[ys, xs] = np.arange(len(ys))
    samp = []
    for q in pieces:
        if len(q) < p['min_piece']:
            continue
        samp.append(q[::p['radial_every']] if len(q) >= p['radial_every']
                    else q[len(q) // 2:len(q) // 2 + 1])
    samp = np.concatenate(samp) if samp else np.zeros(0, np.int64)
    samp = samp[np.linalg.norm(T[samp], axis=1) > 0.5]
    px, py = xs[samp].astype(float), ys[samp].astype(float)
    nx, ny = T[samp, 1], -T[samp, 0]            # the tangent turned a quarter
    flip = (nx * (px - cx) + ny * (py - cy)) < 0
    nx[flip], ny[flip] = -nx[flip], -ny[flip]
    nsteps = int(p['max_step'] / p['march_step'])
    tt = np.arange(1, nsteps + 1) * p['march_step']
    X = np.rint(px[:, None] + nx[:, None] * tt[None]).astype(int)
    Y = np.rint(py[:, None] + ny[:, None] * tt[None]).astype(int)
    inb = (X >= 0) & (X < W) & (Y >= 0) & (Y < H)
    prof = np.zeros(X.shape, bool)
    prof[inb] = S[Y[inb], X[inb]]
    rows = []
    for i in range(len(samp)):
        pr = prof[i]
        off = np.flatnonzero(~pr)              # leave the own sheet, then find the next run
        if not len(off):
            continue
        a = off[0]
        on = np.flatnonzero(pr[a:])
        if not len(on):
            continue
        s0 = a + on[0]
        rest = np.flatnonzero(~pr[s0:])
        s1 = s0 + (rest[0] if len(rest) else len(pr) - s0)
        mid = min(int(0.5 * (s0 + s1 - 1)), nsteps - 1)
        cxp = int(round(px[i] + nx[i] * tt[mid]))
        cyp = int(round(py[i] + ny[i] * tt[mid]))
        if not (0 <= cxp < W and 0 <= cyp < H) or dist[cyp, cxp] > p['skel_snap']:
            continue
        j = idx[iy[cyp, cxp], ix[cyp, cxp]]
        if j < 0 or pid[j] == pid[samp[i]]:
            continue
        rows.append((samp[i], j, tt[s0] - tt[a], (s1 - s0) * p['march_step'],
                     float(np.hypot(xs[j] - px[i], ys[j] - py[i]))))
    return np.asarray(rows, dtype=float).reshape(-1, 5)


# ---------------------------------------------------------------- the solve

def solve(npieces, cont, rad):
    """Integer labels minimizing the convex costs, exactly (HiGHS).

    cont: (a, b, shift, cost per unit). rad: (a, b, f0, f1, f2, f3, s), the
    cost of label difference d = s + 0, s + 1, s + 2, s + 3 (convex, extended
    linearly beyond), s the start-ray shift of the step outward. Returns
    (label per piece, NaN where a piece has no edge; component per piece, -1
    there; solver info). Each component's first piece is at 0."""
    rad = [r for r in rad if max(r[2:6]) - min(r[2:6]) > 0]
    used = np.zeros(npieces, bool)
    for e in list(cont) + list(rad):
        used[int(e[0])] = used[int(e[1])] = True
    var = -np.ones(npieces, np.int64)
    var[used] = np.arange(used.sum())
    nv = int(used.sum())
    ce = [(var[a], var[b], s, w) for a, b, s, w in cont]
    re = [(var[int(r[0])], var[int(r[1])], np.asarray(r[2:6], float), int(r[6])) for r in rad]
    ne = len(ce) + len(re)
    rows, cols, vals, rhs = [], [], [], []

    def add(terms, b):
        r = len(rhs)
        for c, v in terms:
            rows.append(r)
            cols.append(c)
            vals.append(v)
        rhs.append(b)

    e = nv
    for a, b, s, w in ce:                       # t >= w |d - s|
        add([(b, w), (a, -w), (e, -1.0)], w * s)
        add([(b, -w), (a, w), (e, -1.0)], -w * s)
        e += 1
    for a, b, f, sh in re:                      # t >= f(k) + (f(k + 1) - f(k)) (d - sh - k)
        f = f - f.min()
        for k in range(3):
            sk = float(f[k + 1] - f[k])
            if sk == 0 and f[k] == 0:
                continue
            add([(b, sk), (a, -sk), (e, -1.0)], sk * (k + sh) - float(f[k]))
        e += 1
    N = nv + ne
    A = sparse.csr_matrix((vals, (rows, cols)), shape=(len(rhs), N))
    G = sparse.coo_matrix((np.ones(ne), ([x[0] for x in ce] + [x[0] for x in re],
                                         [x[1] for x in ce] + [x[1] for x in re])), shape=(nv, nv))
    ncomp, comp = csgraph.connected_components(G, directed=False)
    bounds = [(None, None)] * nv + [(0, None)] * ne
    seen = set()
    for v in range(nv):
        if comp[v] not in seen:
            seen.add(comp[v])
            bounds[v] = (0, 0)
    t0 = time.time()
    lab = np.full(npieces, np.nan)
    comp_p = -np.ones(npieces, np.int64)
    info = {'variables': N, 'constraints': len(rhs), 'components': int(ncomp)}
    if nv == 0:
        info.update(status=0, message='no edges', seconds=0.0, objective=0.0, max_fractional=0.0)
        return lab, comp_p, info
    res = linprog(np.r_[np.zeros(nv), np.ones(ne)], A_ub=A, b_ub=np.asarray(rhs), bounds=bounds,
                  method='highs')
    info.update(status=int(res.status), message=str(res.message), seconds=round(time.time() - t0, 1),
                objective=float(res.fun) if res.status == 0 else None)
    if res.status == 0:
        x = res.x[:nv]
        info['max_fractional'] = float(np.abs(x - np.rint(x)).max())
        lab[used] = np.rint(x)
        comp_p[used] = comp
    return lab, comp_p, info


def row_costs(n, p=PARAMS):
    """Per radial sample, the cost of label difference d = 0, 1, 2, 3."""
    F = np.zeros((n, 4))
    F[:, 0] = p['c_hi']
    F[:, 2] = p['c_lo']
    F[:, 3] = 2 * p['c_lo']
    return F


def row_shifts(geo):
    """Per radial sample, the start-ray shift of its step outward (+1: the
    next sheet's label is 2 higher, not 1; -1: 0 higher)."""
    rows = geo['rows']
    if not len(rows):
        return np.zeros(0, np.int64)
    th = geo['theta_p']
    d = th[rows[:, 1].astype(int)] - th[rows[:, 0].astype(int)]
    return np.where(d < -math.pi, 1, np.where(d > math.pi, -1, 0)).astype(np.int64)


def aggregate(rows, pid, F, shift):
    """Radial samples to (piece a, piece b, f0, f1, f2, f3, s): the summed
    costs of each piece pair and shift."""
    if not len(rows):
        return []
    a = pid[rows[:, 0].astype(int)]
    b = pid[rows[:, 1].astype(int)]
    sh = np.asarray(shift, np.int64)
    key = (a * (pid.max() + 1) + b) * 3 + (sh + 1)
    u, inv = np.unique(key, return_inverse=True)
    S = np.stack([np.bincount(inv, weights=F[:, k], minlength=len(u)) for k in range(4)], 1)
    first = np.zeros(len(u), np.int64)
    first[inv] = np.arange(len(a))
    return [(int(a[i]), int(b[i]), *(float(v) for v in S[j]), int(sh[i])) for j, i in enumerate(first)]


# ---------------------------------------------------------------- hand

def track_hand(S, cx, cy, p=PARAMS):
    """The spiral's hand from the cleaned slice alone. On rays every
    360 / track_rays degrees from the umbilicus, take the centers of thin
    sheet runs; from a start run, follow the nearest run ray by ray in the
    +atan2 direction for one full turn. Back on the start ray, a spiral whose
    radius grows with the angle lands one run outward (+1), one that shrinks
    one run inward (-1). Returns {hand, outward, inward, tracks}."""
    H, W = S.shape
    nr = int(p['track_rays'])
    tt = np.arange(p['r_min'], p['track_r_max'], 0.5)
    cent = []
    for k in range(nr):
        a = 2 * math.pi * k / nr
        X = np.rint(cx + math.cos(a) * tt).astype(int)
        Y = np.rint(cy + math.sin(a) * tt).astype(int)
        inb = (X >= 0) & (X < W) & (Y >= 0) & (Y < H)
        v = np.zeros(len(tt), bool)
        v[inb] = S[Y[inb], X[inb]]
        st = np.flatnonzero(v[1:] & ~v[:-1]) + 1
        en = np.flatnonzero(~v[1:] & v[:-1]) + 1
        if len(v) and v[0]:
            st = np.r_[0, st]
        if len(v) and v[-1]:
            en = np.r_[en, len(v) - 1]
        n = min(len(st), len(en))
        st, en = st[:n], np.minimum(en[:n], len(tt) - 1)
        c = 0.5 * (tt[st] + tt[en])
        cent.append(c[(tt[en] - tt[st]) <= p['track_max_width']])
    steps = []
    for s0 in range(0, nr, max(1, nr // 24)):
        c0 = cent[s0]
        for i0, r0 in enumerate(c0):
            if not (p['track_r_lo'] <= r0 <= p['track_r_hi']):
                continue
            cur, vel, miss, ok = r0, 0.0, 0, True
            for k in range(1, nr + 1):
                c = cent[(s0 + k) % nr]
                pred = cur + vel
                if len(c):
                    j = int(np.argmin(np.abs(c - pred)))
                    if abs(c[j] - pred) < 3.0:
                        vel = 0.5 * vel + 0.5 * (c[j] - cur)
                        cur, miss = c[j], 0
                        continue
                miss += 1
                cur = pred
                if miss > 12:
                    ok = False
                    break
            if not ok:
                continue
            j = int(np.argmin(np.abs(c0 - cur)))
            if abs(c0[j] - cur) <= 4:
                steps.append(j - i0)
    steps = np.asarray(steps, int)
    out_, in_ = int((steps == 1).sum()), int((steps == -1).sum())
    return {'hand': 1 if out_ >= in_ else -1, 'outward': out_, 'inward': in_, 'tracks': int(len(steps))}


def hand_of_sense(sense):
    """+1 for CW (radius grows with atan2(y, x)), -1 for ACW, None for
    abstain, from a call or an ``outward_sense`` result file."""
    if sense is None:
        return None
    s = str(sense)
    if s.upper() not in ('CW', 'ACW', 'ABSTAIN'):
        with open(s) as fh:
            s = json.load(fh)['call']
    s = s.upper()
    return 1 if s == 'CW' else (-1 if s == 'ACW' else None)


def flip_hand(geo):
    """The same geometry with the other hand: theta' -> mod(-theta'), same
    start ray, so the pieces stay as cut; continuity shifts are recomputed
    from their end pixels."""
    g = dict(geo)
    th = np.mod(-geo['theta_p'], 2 * math.pi)
    th[th >= 2 * math.pi] = 0.0
    g['theta_p'] = th
    g['hand'] = -int(geo['hand'])
    pc = geo['pieces']
    g['cont'] = [(a, b, ray_shift(th[pc[b][0]] - th[pc[a][-1]])) for a, b, _ in geo['cont']]
    return g


# ---------------------------------------------------------------- one slice

def geometry(m7, cx, cy, p=PARAMS, log=print):
    """Everything that does not depend on the costs: skeleton, pieces,
    continuity edges, radial samples. The hand here is a shape statistic
    (radius against angle along long paths); label_geometry replaces it."""
    t0 = time.time()
    S = clean_slice(m7, p)
    K = thin(S)
    ys, xs = np.nonzero(K)
    near = np.hypot(xs - cx, ys - cy) < p['r_min']
    K[ys[near], xs[near]] = False
    if p.get('spur_len', 0):
        K = prune_spurs(K, int(p['spur_len']), int(p.get('spur_rounds', 2)))
    ys, xs, A, _ = skel_graph(K)
    deg = np.asarray(A.sum(axis=1)).ravel()
    r = np.hypot(xs - cx, ys - cy)
    th = np.arctan2(ys - cy, xs - cx)
    pl = paths(A, deg)
    T = tangents(ys, xs, pl, p['tangent_half'])
    num = 0.0
    path_len = np.zeros(len(ys))
    for q in pl:
        path_len[q] = len(q)
        if len(q) >= 20:
            num += float((np.diff(r[q]) * np.diff(np.unwrap(th[q]))).sum())
    hand = 1 if num >= 0 else -1
    thp = np.mod(hand * th, 2 * math.pi)
    pid, pieces, cont = make_pieces(pl, thp, len(ys), p)
    rows = radial_edges(S, K, ys, xs, pid, pieces, T, cx, cy, p)
    log(f'skeleton {len(ys)} px, {len(pieces)} pieces, {len(cont)} continuity edges, '
        f'{len(rows)} radial samples, {time.time() - t0:.0f} s')
    return {'S': S, 'ys': ys, 'xs': xs, 'pid': pid, 'pieces': pieces, 'cont': cont, 'rows': rows,
            'path_len': path_len, 'hand': hand, 'theta_p': thp, 'r': r}


def label_geometry(geo, hand, p=PARAMS, log=print):
    """Labels of one slice with the given hand (+1 or -1). The returned
    ``geo`` is the geometry actually used (theta', hand, continuity shifts)."""
    if int(hand) != int(geo['hand']):
        geo = flip_hand(geo)
    rad = aggregate(geo['rows'], geo['pid'], row_costs(len(geo['rows']), p), row_shifts(geo))
    cont = [(a, b, s, p['c_cont']) for a, b, s in geo['cont']]
    lab, comp, info = solve(len(geo['pieces']), cont, rad)
    info['hand'] = int(geo['hand'])
    log(f'solve {info}')
    return {'rad': rad, 'lab_piece': lab, 'comp_piece': comp, 'label': lab[geo['pid']],
            'comp': comp[geo['pid']], 'info': info, 'geo': geo}


def label_slice(m7, cx, cy, hand=None, p=PARAMS, log=print):
    """Geometry and labels of one slice; hand None tracks it on the slice."""
    geo = geometry(m7, cx, cy, p, log)
    tracked = None
    if hand is None:
        tracked = track_hand(geo['S'], cx, cy, p)
        hand = tracked['hand']
    R = label_geometry(geo, hand, p, log)
    R['tracked_hand'] = tracked
    return R


# ---------------------------------------------------------------- volume and files

def read_slice(arr, z, level=0):
    """The whole level-``level`` plane at level-0 z."""
    return np.asarray(arr[int(z) // 2 ** level])


def save_slice(out, z, R, scale=1):
    """zNNNNN.npz: skeleton pixels (x, y in plane pixels; ``scale`` level-0
    voxels per plane pixel) with piece, label, component, theta', radius and
    the hand, plus radial sample rows and continuity edges (a, b, shift)."""
    g = R['geo']
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(out / f'z{int(z):05d}.npz', ys=g['ys'], xs=g['xs'], label=R['label'],
                        comp=R['comp'], pid=g['pid'], theta_p=g['theta_p'], r=g['r'],
                        hand=np.int64(g['hand']), rows=g['rows'],
                        cont=np.asarray(g['cont'], np.int64).reshape(-1, 3),
                        z=np.int64(z), scale=np.float64(scale))


def label_one(z, src, level, umb, hand, p, out):
    t0 = time.time()
    arr = open_volume(src, level)
    s = 2 ** level
    cx, cy = center_at(umb, z)
    R = label_slice(read_slice(arr, z, level), cx / s, cy / s, hand, p, log=lambda m: None)
    save_slice(out, z, R, s)
    return {'z': int(z), 'hand': R['info']['hand'], 'tracked_hand': R['tracked_hand'],
            'axis': [cx, cy], 'pieces': len(R['geo']['pieces']), 'radial_terms': len(R['rad']),
            'solve': R['info'], 'seconds': round(time.time() - t0, 1)}


# ---------------------------------------------------------------- command line

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--prediction', required=True, help='binary surface prediction zarr (array or multiscale group)')
    ap.add_argument('--level', type=int, default=0, help='pyramid level read (default %(default)s)')
    ap.add_argument('--umbilicus', required=True, help="umbilicus JSON or text, in the prediction's level-0 voxels")
    ap.add_argument('--z', type=int, nargs='+', required=True, help='level-0 z of the slices to label')
    ap.add_argument('--sense', default=None, help='CW, ACW or an outward_sense result file; without it, or '
                    'when it abstains, the hand is tracked on each slice')
    ap.add_argument('--params', default=None, help='JSON overrides of PARAMS')
    ap.add_argument('--jobs', type=int, default=1)
    ap.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    p = dict(PARAMS)
    if a.params:
        extra = json.loads(a.params)
        unknown = sorted(set(extra) - set(PARAMS))
        if unknown:
            ap.error(f'unknown params {unknown}')
        p.update(extra)
    hand = hand_of_sense(a.sense)
    umb = load_umbilicus(a.umbilicus)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'params.json').write_text(json.dumps({'params': p, 'sense': a.sense, 'hand': hand,
                                                 'prediction': a.prediction, 'level': a.level}, indent=1))
    args = [(z, a.prediction, a.level, umb, hand, p, out) for z in a.z]
    if a.jobs > 1:
        with concurrent.futures.ProcessPoolExecutor(a.jobs) as pool:
            results = list(pool.map(label_one, *zip(*args)))
    else:
        results = [label_one(*x) for x in args]
    with open(out / 'runs.jsonl', 'a') as fh:
        for r in results:
            fh.write(json.dumps(r) + '\n')
            print(json.dumps({k: r[k] for k in ('z', 'hand', 'pieces', 'seconds')}), flush=True)
    bad = [r['z'] for r in results if r['solve']['status'] != 0]
    if bad:
        print(f'label_slices: no optimum on z {bad}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
