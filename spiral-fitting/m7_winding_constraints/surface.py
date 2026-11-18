"""Volume opening and neighbor search for the winding constraints.

``open_volume`` and ``pairs_within`` are the same functions as in
``band_unroll/surface.py``, so ``outward_sense.py`` here and there stay one
code. Only numpy and zarr are needed.
"""

import numpy as np


def open_volume(src, level=None):
    """A zarr array from a path or URL (``level`` picks an array of a
    multiscale group), or a numpy array as is."""
    if isinstance(src, np.ndarray):
        return src
    import zarr
    node = zarr.open(str(src), mode='r')
    if level is not None:
        node = node[str(level)]
    if not hasattr(node, 'shape'):
        raise ValueError(f'{src}: a group; pass level= to pick an array')
    return node


def pairs_within(A, B, r, chunk=200000):
    """(ia, ib): every pair with |A[ia] - B[ib]| <= r, by a spatial hash with
    cells of size r. Works in any dimension; no scipy."""
    A = np.asarray(A, dtype=np.float64)
    B = np.asarray(B, dtype=np.float64)
    dim = A.shape[1] if A.ndim == 2 else B.shape[1]
    A, B = A.reshape(-1, dim), B.reshape(-1, dim)
    empty = np.zeros(0, np.int64)
    if len(A) == 0 or len(B) == 0 or r <= 0:
        return empty, empty
    lo = np.minimum(A.min(0), B.min(0)) - r
    cb = np.floor((B - lo) / r).astype(np.int64)
    ca = np.floor((A - lo) / r).astype(np.int64)
    span = int(max(cb.max(), ca.max())) + 3
    if float(span) ** dim >= 2.0 ** 62:
        raise ValueError('point extent too large for the neighbor hash')

    def key(c):
        k = np.zeros(len(c), np.int64)
        for i in range(dim):
            k = k * span + c[:, i]
        return k

    kb = key(cb)
    order = np.argsort(kb, kind='stable')
    uk, start, count = np.unique(kb[order], return_index=True, return_counts=True)
    offsets = np.stack(np.meshgrid(*([np.array([-1, 0, 1])] * dim), indexing='ij'),
                       -1).reshape(-1, dim)
    out_a, out_b = [], []
    for c0 in range(0, len(A), chunk):
        ca_c, Ac = ca[c0:c0 + chunk], A[c0:c0 + chunk]
        for off in offsets:
            q = key(ca_c + off)
            pos = np.clip(np.searchsorted(uk, q), 0, len(uk) - 1)
            hit = uk[pos] == q
            if not hit.any():
                continue
            ai = np.flatnonzero(hit)
            cnt, st = count[pos[ai]], start[pos[ai]]
            rep_a = np.repeat(ai, cnt)
            first = np.repeat(np.cumsum(cnt) - cnt, cnt)
            bi = order[np.repeat(st, cnt) + (np.arange(len(rep_a)) - first)]
            k = ((Ac[rep_a] - B[bi]) ** 2).sum(1) <= r * r
            out_a.append(rep_a[k] + c0)
            out_b.append(bi[k])
    if not out_a:
        return empty, empty
    return np.concatenate(out_a), np.concatenate(out_b)
