"""Write the surest slice labels as relative winding constraints for the fit.

Model output, never a reading.

    python -m m7_winding_constraints.ladders --slices slices/ --out constraints/ [--matrix M.json] \\
        [--fit-scale 4] [--control]

``fit_spiral.py`` takes relative windings: point collections whose points
carry ``wind_a``, the number of windings each lies outward of the others in
its collection (``winding_is_absolute`` false), ingested into the object store
with ``vc_object_ingest --point-collections``. ``label_slices.py`` labels
every skeleton piece of a z slice in one solve; this keeps only the labels it
is surest of and writes them in that format. It never looks at ink, and it
adds no new label: every ``wind_a`` is a slice label.

For each slice file (``zNNNNN.npz``):

    bad     the pieces touching a violated edge: a continuity edge whose label
            difference is not its shift, or a radial edge (the next sheet out)
            whose difference is not 1 plus the start-ray shift
    sure    a piece is sure when its graph distance, over the slice's
            continuity and radial edges, to the nearest bad piece is at least
            D_SURE (3); a slice with no bad piece makes every piece sure
    main    each connected component of the slice's graph was solved on its
            own offset, so labels compare only inside one component. A
            slice's collection holds the sure pieces of one component, the one
            with the most sure pieces (the smallest component number on a
            tie); the sure pieces of the others are counted and left out
    points  up to K (8) pixels per sure piece, spread evenly along it (both
            ends kept), mapped to the fit volume's voxels
            (fit = (M (x, y, z, 1)) / fit_scale, x and y in the prediction's
            level-0 voxels; M is the identity unless --matrix gives a 3 x 4
            affine, such as a catalog transform from the prediction's scan to
            the fit's), rounded to 0.001
    wind_a  the piece's label minus the smallest label written for the slice
    order   points are numbered by theta' ascending (then r): the fit walks a
            collection's points in id order and unwraps the angle between
            neighbors, so the walk never crosses the start ray, and a slice
            whose neighboring points are pi or more apart is refused

One collection per slice. With ``--control``, the plus-one control is written
beside it: the same points with every step between neighboring winding
levels made one winding larger. A fit taught by the control should score
worse than one taught by the constraints; if it does not, the gain is not
coming from the windings.

Needs numpy and scipy.
"""

import argparse
import hashlib
import json
import math
import sys
from pathlib import Path

import numpy as np
from scipy import sparse
from scipy.sparse import csgraph

D_SURE = 3
K = 8
VERSION = '1.2.0'


class LadderError(ValueError):
    pass


# ---------------------------------------------------------------- the sure rule

def sure_distance(d):
    """(dist, bad, lp) of one slice's labeling d (a zNNNNN.npz as a dict).
    lp is each piece's label; bad marks pieces touching a violated continuity
    edge (difference != shift) or radial edge (difference != 1 + start-ray
    shift); dist is the graph distance in pieces (continuity and radial
    adjacency) to the nearest bad piece, inf where none is reachable."""
    L, th, pid = np.asarray(d['label'], float), np.asarray(d['theta_p'], float), np.asarray(d['pid'])
    cont, rows = np.asarray(d['cont']).reshape(-1, 3), np.asarray(d['rows'])
    npc = int(pid.max()) + 1
    lp = np.full(npc, np.nan)
    lp[pid[pid >= 0]] = L[pid >= 0]
    a = rows[:, 0].astype(np.int64)
    b = rows[:, 1].astype(np.int64)
    dth = th[b] - th[a]
    s = np.where(dth < -math.pi, 1, np.where(dth > math.pi, -1, 0))
    pa, pb = pid[a], pid[b]
    ok = (pa >= 0) & (pb >= 0)            # a radial sample off every piece is left out
    pa, pb, s = pa[ok], pb[ok], s[ok]
    rv = (lp[pb] - lp[pa]) != 1 + s
    cv = (lp[cont[:, 1]] - lp[cont[:, 0]]) != cont[:, 2]
    bad = np.zeros(npc, bool)
    bad[pa[rv]] = True
    bad[pb[rv]] = True
    bad[cont[cv, 0]] = True
    bad[cont[cv, 1]] = True
    E = np.concatenate([np.stack([pa, pb], 1), cont[:, :2]]).astype(np.int64)
    G = sparse.coo_matrix((np.ones(len(E)), (E[:, 0], E[:, 1])), shape=(npc, npc)).tocsr()
    G = G + G.T
    src = np.flatnonzero(bad)
    dist = (csgraph.dijkstra(G, unweighted=True, indices=src, min_only=True) if len(src)
            else np.full(npc, np.inf))
    return np.asarray(dist, float), bad, lp


def sure_mask(d, d_sure=D_SURE):
    """(sure, has_px, dist, bad, lp): sure pieces are at least d_sure from
    every bad piece, labeled and on a pixel."""
    dist, bad, lp = sure_distance(d)
    pid = np.asarray(d['pid'])
    has_px = np.zeros(len(lp), bool)
    has_px[pid[pid >= 0]] = True
    return (dist >= d_sure) & np.isfinite(lp) & has_px, has_px, dist, bad, lp


def piece_component(d, npc):
    """Per piece, the component of the slice's graph it was solved in (-1
    where the piece has no pixel or no label)."""
    if 'comp' not in d:
        raise LadderError('the slice file has no comp array, so its labels cannot be told apart by component')
    pid = np.asarray(d['pid'])
    out = np.full(npc, -1, np.int64)
    out[pid[pid >= 0]] = np.asarray(d['comp']).astype(np.int64)[pid >= 0]
    return out


def main_component(d, sure):
    """(written, same, component, components) for one slice: labels of two
    components share no offset, so one collection takes one component, the
    one holding the most sure pieces (the smallest component number on a
    tie). written marks its sure pieces, same every piece of it, components
    counts the components holding a sure piece."""
    sure = np.asarray(sure, bool)
    pc = piece_component(d, len(sure))
    if (pc[sure] < 0).any():
        raise LadderError('a sure piece has no component')
    ids, counts = np.unique(pc[sure], return_counts=True)
    component = int(ids[np.argmax(counts)])
    same = pc == component
    return sure & same, same, component, int(len(ids))


# ---------------------------------------------------------------- points

def _invert(A):
    Li = np.linalg.inv(A[:, :3])
    return np.concatenate([Li, (-Li @ A[:, 3])[:, None]], axis=1)


class Frame:
    """fit = (M[:, :3] q + M[:, 3]) / scale, for q = (x, y, z) in the
    prediction's level-0 voxels. The fit-to-prediction affine is built first
    and then inverted, as in the tests this rule was frozen for, so points
    round to 0.001 the same way."""

    def __init__(self, matrix=None, scale=1.0):
        self.M = np.asarray(np.eye(3, 4) if matrix is None else matrix, float)
        if self.M.shape != (3, 4):
            raise LadderError(f'the matrix is {self.M.shape}, not 3 x 4')
        self.scale = float(scale)
        S = np.zeros((3, 4))
        S[[0, 1, 2], [0, 1, 2]] = self.scale
        Mi = _invert(self.M)
        to_query = np.concatenate([Mi[:, :3] @ S[:, :3], (Mi[:, :3] @ S[:, 3] + Mi[:, 3])[:, None]], axis=1)
        self.Ainv = _invert(to_query)

    def to_fit(self, q):
        q = np.asarray(q, float)
        return q @ self.Ainv[:, :3].T + self.Ainv[:, 3]


def spread_pick(n, k=K):
    """Up to k positions spread evenly over 0 .. n - 1 (both ends included)."""
    if n <= 0:
        return np.zeros(0, np.int64)
    return np.unique(np.rint(np.linspace(0, n - 1, min(k, n))).astype(np.int64))


def piece_pixels(pid, theta, r):
    """{piece: pixel indices ordered along the piece (theta', then r)};
    pieces never cross the start ray."""
    order = np.lexsort((r, theta, pid))
    p = pid[order]
    keep = p >= 0
    order, p = order[keep], p[keep]
    cuts = np.flatnonzero(np.diff(p)) + 1
    return {int(g[0]): o for g, o in zip(np.split(p, cuts), np.split(order, cuts)) if len(g)}


def slice_points(d, z, frame, pieces, lp, base, k=K):
    """The points of the given pieces on slice z: p (fit xyz, rounded to
    0.001), wind_a, theta', r and piece, numbered by theta' ascending; None
    when no piece has a pixel."""
    pid = np.asarray(d['pid'])
    th = np.asarray(d['theta_p'], float)
    r = np.asarray(d['r'], float) if 'r' in d else np.zeros(len(pid))
    scale = float(d['scale']) if 'scale' in d else 1.0
    px = piece_pixels(pid, th, r)
    idx, wa, pc = [], [], []
    for q in sorted(int(x) for x in pieces):
        o = px.get(q)
        if o is None:
            continue
        sel = o[spread_pick(len(o), k)]
        idx.append(sel)
        wa.append(np.full(len(sel), float(lp[q] - base)))
        pc.append(np.full(len(sel), q))
    if not idx:
        return None
    idx, wa, pc = np.concatenate(idx), np.concatenate(wa), np.concatenate(pc)
    xs, ys = np.asarray(d['xs'], float)[idx] * scale, np.asarray(d['ys'], float)[idx] * scale
    q3 = np.stack([xs, ys, np.full(len(idx), float(z))], 1)
    fit = np.round(np.asarray(frame.to_fit(q3), float), 3)
    order = np.lexsort((r[idx], th[idx]))
    return {'p': fit[order], 'wind_a': wa[order], 'theta': th[idx][order], 'r': r[idx][order],
            'piece': pc[order]}


def max_gap(theta):
    """Largest angle between neighboring points of the walk (theta' ascending)."""
    t = np.sort(np.asarray(theta, float))
    return float(np.diff(t).max()) if len(t) > 1 else 0.0


def collection(pts, z, hand, next_id, extra=None, k=K, d_sure=D_SURE, name='m7-winding'):
    """One villa point collection: points keyed by ids unique in the document,
    each with ``p`` (x, y, z) and ``wind_a``; relative (winding_is_absolute
    false at the collection's top level, where the fit reads it)."""
    points = {}
    for j in range(len(pts['wind_a'])):
        points[str(next_id + j)] = {'p': [float(v) for v in pts['p'][j]], 'wind_a': float(pts['wind_a'][j]),
                                    'creation_time': 0}
    md = {'generator': f'm7_winding_constraints {VERSION}: sure pieces (D_SURE {d_sure}, K {k}), '
                       "points by theta' ascending", 'z': int(z), 'hand': int(hand)}
    md.update(extra or {})
    return {'name': f'{name}-z{int(z):05d}', 'winding_is_absolute': False, 'metadata': md, 'points': points}


# ---------------------------------------------------------------- constraints

def load_slices(folder, zs=None):
    """{z: slice dict} of a label_slices output folder: the given z, or every
    zNNNNN.npz in it."""
    folder = Path(folder)
    if zs is None:
        zs = sorted(int(f.stem[1:]) for f in folder.glob('z*.npz'))
    if not zs:
        raise LadderError(f'no zNNNNN.npz in {folder}')
    out = {}
    for z in zs:
        f = folder / f'z{int(z):05d}.npz'
        if not f.is_file():
            raise LadderError(f'missing {f}')
        with np.load(f) as d:
            out[int(z)] = {key: d[key] for key in d.files}
    return out


def build(slices, frame, k=K, d_sure=D_SURE):
    """The constraints over the slices: (point collections document, record).
    Refuses a slice without a sure piece or a walk with a step of pi or more."""
    cols, nid = {}, 1
    rec = {'D_SURE': d_sure, 'K': k, 'slices': {}}
    for n, z in enumerate(sorted(slices), start=1):
        d = slices[z]
        sure, has_px, dist, bad, lp = sure_mask(d, d_sure)
        if not sure.any():
            raise LadderError(f'z {z}: no sure piece')
        try:
            written, same, component, ncomp = main_component(d, sure)
        except LadderError as e:
            raise LadderError(f'z {z}: {e}') from None
        base = float(np.nanmin(lp[written])) + 0.0
        pts = slice_points(d, z, frame, np.flatnonzero(written), lp, base, k)
        gap = max_gap(pts['theta'])
        if gap >= math.pi:
            raise LadderError(f'z {z}: neighboring points {gap:.2f} rad apart (the fit needs under pi)')
        cols[str(n)] = collection(pts, z, int(d['hand']), nid, {'base_label': base, 'component': component},
                                  k, d_sure)
        nid += len(pts['wind_a'])
        lv = np.unique(pts['wind_a'])
        rec['slices'][str(z)] = {
            'pieces': int(has_px.sum()), 'bad': int(bad.sum()), 'sure': int(sure.sum()),
            'written': int(written.sum()), 'sure_left_out': int(sure.sum() - written.sum()),
            'component': component, 'components_with_sure': ncomp,
            'sure_pixels': int(np.isin(d['pid'], np.flatnonzero(sure)).sum()),
            'pixels': int((np.asarray(d['pid']) >= 0).sum()), 'points': int(len(pts['wind_a'])),
            'levels': int(len(lv)), 'span': float(lv.max() - lv.min()), 'base_label': base,
            'hand': int(d['hand']), 'max_theta_gap': round(gap, 4)}
    doc = {'vc_pointcollections_json_version': '1', 'collections': cols}
    rec['points'] = sum(v['points'] for v in rec['slices'].values())
    rec['collections'] = len(cols)
    rec['sure_left_out'] = sum(v['sure_left_out'] for v in rec['slices'].values())
    return doc, rec


def plus_one(doc):
    """The control: on each collection's distinct winding levels, every step
    between neighboring levels gains one winding; points of one level keep
    one level."""
    out = json.loads(json.dumps(doc))
    for col in out['collections'].values():
        lv = sorted({float(p['wind_a']) for p in col['points'].values()})
        new, acc = {lv[0]: lv[0]}, 0.0
        for i in range(1, len(lv)):
            acc += lv[i] - lv[i - 1] + 1
            new[lv[i]] = acc + lv[0]
        for p in col['points'].values():
            p['wind_a'] = new[float(p['wind_a'])]
        col['name'] = col['name'].replace('m7-winding-', 'm7-winding-control-')
        col['metadata'] = dict(col['metadata'], control='plus_one on the winding levels')
    return out


# ---------------------------------------------------------------- command line

def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as fh:
        for b in iter(lambda: fh.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def write_json(path, obj):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, separators=(',', ':')) + '\n')
    return sha256_file(path)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--slices', required=True, help="label_slices.py's output folder (its zNNNNN.npz files)")
    ap.add_argument('--out', required=True, help='output folder (created)')
    ap.add_argument('--z', type=int, nargs='*', default=None, help='the slices to use (default: every file)')
    ap.add_argument('--matrix', help='JSON {"matrix": 3 x 4} from the prediction\'s level-0 voxels to the fit '
                    "volume's full-resolution voxels (default identity)")
    ap.add_argument('--fit-scale', type=float, default=1.0,
                    help='full-resolution voxels per fit-volume voxel (default %(default)s)')
    ap.add_argument('--k', type=int, default=K, help='points per sure piece (default %(default)s)')
    ap.add_argument('--d-sure', type=int, default=D_SURE, help='sure distance in pieces (default %(default)s)')
    ap.add_argument('--control', action='store_true', help='also write the plus-one control')
    a = ap.parse_args(argv)
    if a.k < 1 or a.d_sure < 1:
        ap.error('--k and --d-sure must be at least 1')
    try:
        M = json.loads(Path(a.matrix).read_text())['matrix'] if a.matrix else None
        slices = load_slices(a.slices, a.z)
        doc, rec = build(slices, Frame(M, a.fit_scale), a.k, a.d_sure)
    except LadderError as e:
        print(f'ladders: {e}', file=sys.stderr)
        return 2
    out = Path(a.out)
    rec['version'] = VERSION
    rec['files'] = {'constraints': {'file': 'relative_windings.json',
                                    'sha256': write_json(out / 'relative_windings.json', doc)}}
    if a.control:
        rec['files']['control'] = {'file': 'relative_windings_control.json',
                                   'sha256': write_json(out / 'relative_windings_control.json', plus_one(doc))}
    rec['slice_files'] = {f'z{z:05d}.npz': sha256_file(Path(a.slices) / f'z{z:05d}.npz') for z in sorted(slices)}
    (out / 'ladders.json').write_text(json.dumps(rec, indent=1) + '\n')
    print(json.dumps({k: rec[k] for k in ('points', 'collections', 'sure_left_out', 'files')}), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
