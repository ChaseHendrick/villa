"""m7_winding_constraints on small synthetic spirals (no downloads).

The outward sense is read from single planes and flips with a mirrored plane;
whole slices are labelled with their true windings in both senses, also with
a sheet the prediction missed and holes; the wrong hand leaves violated
edges; the sure rule keeps wrongly labelled pieces and other components out;
the written file follows the point-collection rules the object store's
ingest checks, with ``winding_is_absolute`` false where the fit reads it; the
plus-one control changes every step; and the command line runs end to end on
a local zarr array.
"""

import json
import math
from pathlib import Path

import numpy as np
import pytest

from m7_winding_constraints import label_slices, ladders, outward_sense

CX, CY, R0, PITCH = 352.0, 347.0, 40.0, 16.0
N = 700


def spiral_slice(hand, n=N, width=1.6, turns=18, drop=None, gaps=0, seed=0):
    """A binary slice (0 or 255) of an Archimedean spiral about (CX, CY),
    sheets PITCH apart; hand +1 is CW (radius grows with atan2(y, x)). drop:
    (turn, lo, hi) leaves that turn's sheet out for theta' in [lo, hi); gaps:
    that many small holes."""
    yy, xx = np.mgrid[0:n, 0:n]
    R = np.hypot(xx - CX, yy - CY)
    th = np.mod(hand * np.arctan2(yy - CY, xx - CX), 2 * np.pi)
    u = (R - R0) / PITCH - th / (2 * np.pi)
    k = np.rint(u)
    S = (np.abs(u - k) * PITCH <= width) & (k >= 0) & (R < R0 + PITCH * turns)
    if drop is not None:
        t, lo, hi = drop
        S &= ~((k == t) & (th >= lo) & (th < hi))
    rng = np.random.default_rng(seed)
    for _ in range(gaps):
        y, x = rng.integers(60, n - 60, 2)
        S[y - 3:y + 4, x - 3:x + 4] = False
    return (S * 255).astype(np.uint8)


P = dict(label_slices.PARAMS, r_min=60.0, track_r_max=330.0, track_r_lo=80.0, track_r_hi=300.0,
         track_rays=720)


def true_turn(geo, hand):
    th = np.mod(hand * np.arctan2(geo['ys'] - CY, geo['xs'] - CX), 2 * np.pi)
    return np.rint((geo['r'] - R0) / PITCH - th / (2 * np.pi))


def offset_share(L, k):
    """Share of labelled pixels whose label minus true turn is the most common
    offset."""
    ok = np.isfinite(L)
    _, c = np.unique((L - k)[ok], return_counts=True)
    return c.max() / c.sum()


def as_file(R):
    """A label_slices result as the dict its zNNNNN.npz holds."""
    g = R['geo']
    return {'ys': g['ys'], 'xs': g['xs'], 'label': R['label'], 'comp': R['comp'], 'pid': g['pid'],
            'theta_p': g['theta_p'], 'r': g['r'], 'hand': np.int64(g['hand']), 'rows': g['rows'],
            'cont': np.asarray(g['cont'], np.int64).reshape(-1, 3), 'scale': np.float64(1)}


@pytest.fixture(scope='module')
def labelled():
    return {h: label_slices.label_slice(spiral_slice(h), CX, CY, None, P, log=lambda m: None) for h in (1, -1)}


# ---------------------------------------------------------------- step 0: outward sense

@pytest.mark.parametrize('hand,sense', [(1, 'CW'), (-1, 'ACW')])
def test_outward_sense_from_slices(hand, sense):
    other = {'CW': 'ACW', 'ACW': 'CW'}[sense]
    # the slices labelled below: each full turn of chain votes the planted sense
    img = spiral_slice(hand, gaps=40, seed=1)
    v = outward_sense.plane_votes(img, CX, CY, PITCH, N)
    assert v[0 if sense == 'CW' else 1] >= 2 and v[1 if sense == 'CW' else 0] == 0
    assert outward_sense.plane_votes(img[:, ::-1], N - 1 - CX, CY, PITCH, N) == v[::-1]
    # a plane broken into many chains carries enough votes for a call
    img, cx, cy, pitch = outward_sense.synthetic(sense)
    assert outward_sense.decide(*outward_sense.plane_votes(img, cx, cy, pitch, img.shape[0])) == sense
    cw, acw = outward_sense.plane_votes(img[:, ::-1], img.shape[1] - 1 - cx, cy, pitch, img.shape[0])
    assert outward_sense.decide(cw, acw) == other         # a mirrored plane flips the call
    assert label_slices.hand_of_sense(sense) == hand      # CW is hand +1 in label_slices


def test_outward_sense_abstains_on_rings():
    img, cx, cy, pitch = outward_sense.synthetic('CW', rings=True)
    assert outward_sense.decide(*outward_sense.plane_votes(img, cx, cy, pitch, img.shape[0])) == 'abstain'
    assert label_slices.hand_of_sense('abstain') is None


def test_outward_sense_is_the_band_unroll_code():
    """While both exist, this copy and band_unroll's differ only in the usage
    line, so the two calls stay the same."""
    other = Path(__file__).resolve().parents[1] / 'band_unroll' / 'outward_sense.py'
    if not other.is_file():
        pytest.skip('band_unroll is not in this checkout')
    mine = Path(outward_sense.__file__).read_text().replace('m7_winding_constraints.', 'band_unroll.')
    assert mine == other.read_text()


# ---------------------------------------------------------------- slice labels

def test_both_hands_label_every_turn(labelled):
    for hand, R in labelled.items():
        g = R['geo']
        assert g['hand'] == hand
        assert R['tracked_hand']['hand'] == hand
        assert R['info']['status'] == 0 and R['info']['max_fractional'] == 0.0
        assert np.isfinite(R['label']).mean() > 0.95
        assert offset_share(R['label'], true_turn(g, hand)) > 0.99


def test_missed_sheet_and_gaps():
    # one turn missing over a quarter turn, and holes: the asymmetric radial
    # costs allow the step of 2 there
    m7 = spiral_slice(-1, drop=(9, 1.0, 2.6), gaps=25, seed=4)
    R = label_slices.label_slice(m7, CX, CY, -1, P, log=lambda m: None)
    assert offset_share(R['label'], true_turn(R['geo'], -1)) > 0.97


def test_wrong_hand_leaves_violated_edges(labelled):
    # the check can fail: with the hand reversed, paths cross the start ray
    # with the wrong shift, so the solve cannot satisfy every edge and the
    # sure rule finds bad pieces; with the right hand it finds none
    for hand, R in labelled.items():
        assert R['info']['objective'] == pytest.approx(0.0)
        assert not ladders.sure_mask(as_file(R))[3].any()
        W = label_slices.label_slice(spiral_slice(hand), CX, CY, -hand, P, log=lambda m: None)
        assert W['info']['objective'] > 10
        assert ladders.sure_mask(as_file(W))[3].sum() > 10


def test_solve_radial_step_across_the_start_ray():
    # piece 1 lies one sheet out from piece 0 but across theta' = 0 going up
    # (shift +1): its label must be 2 higher; continuity keeps piece 2 on 1
    rad = [(0, 1, 20.0, 0.0, 1.0, 2.0, 1)]
    lab, comp, info = label_slices.solve(3, [(1, 2, 0, 10.0)], rad)
    assert info['status'] == 0
    assert list(lab) == [0.0, 2.0, 2.0]
    assert list(comp) == [0, 0, 0]


def test_solve_components_have_their_own_offset():
    lab, comp, _ = label_slices.solve(5, [(0, 1, 0, 10.0), (3, 4, 1, 10.0)], [])
    assert lab[0] == 0 and lab[1] == 0 and lab[3] == 0 and lab[4] == 1
    assert np.isnan(lab[2]) and comp[2] == -1
    assert comp[0] != comp[3]


def test_hand_from_a_sense_file(tmp_path):
    f = tmp_path / 'sense.json'
    f.write_text(json.dumps({'call': 'ACW', 'CW': 0, 'ACW': 9}))
    assert label_slices.hand_of_sense(str(f)) == -1
    assert label_slices.hand_of_sense(None) is None


# ---------------------------------------------------------------- constraints

def ring_slice(n_turns=8, per_turn=1200, piece=40, wrong=(), offset=5, island=None, island_offset=37):
    """One slice file without images: a spiral's outward position Q (in turns)
    puts a pixel at angle 2 pi Q and radius 100 + 25 Q; its turn is floor(Q).
    Pieces are runs of 40 pixels inside one turn. ``wrong`` lists pieces
    whose label is raised by one. With ``island``, the pieces from that one
    outward are a second component whose labels are ``island_offset`` too
    large, as the solve leaves them."""
    Q = 3.0 + (np.arange(n_turns * per_turn) + 0.5) / per_turn
    T = np.floor(Q).astype(np.int64)
    theta = 2 * math.pi * (Q - T)
    r = 100.0 + 25.0 * Q
    xs = np.rint(2000 + r * np.cos(theta)).astype(np.int64)
    ys = np.rint(2000 + r * np.sin(theta)).astype(np.int64)
    pid = np.zeros(len(Q), np.int64)
    k, cnt, prev = 0, 0, T[0]
    for i in range(len(Q)):
        if T[i] != prev or cnt == piece:
            k, cnt, prev = k + 1, 0, T[i]
        pid[i] = k
        cnt += 1
    npc = k + 1
    plabel = np.array([T[pid == q][0] + offset for q in range(npc)], np.int64)
    for q in wrong:
        plabel[q] += 1
    out = np.arange(npc) >= (npc if island is None else island)
    plabel[out] += island_offset
    cont = []
    for q in range(npc - 1):
        if out[q] != out[q + 1]:
            continue
        a, b = np.flatnonzero(pid == q)[-1], np.flatnonzero(pid == q + 1)[0]
        cont.append((q, q + 1, int(T[b] - T[a])))
    rows = np.stack([np.arange(0, len(Q) - per_turn, 17), np.arange(0, len(Q) - per_turn, 17) + per_turn], 1)
    rows = rows[out[pid[rows[:, 0]]] == out[pid[rows[:, 1]]]]
    d = {'ys': ys, 'xs': xs, 'label': plabel[pid].astype(float), 'pid': pid, 'theta_p': theta, 'r': r,
         'hand': np.int64(1), 'comp': out[pid].astype(np.int64), 'rows': rows.astype(np.int64),
         'cont': np.asarray(cont, np.int64).reshape(-1, 3)}
    return d, T


def test_sure_rule_keeps_wrong_pieces_out():
    d, T = ring_slice(wrong=(60, 61, 62))
    sure, has_px, dist, bad, lp = ladders.sure_mask(d)
    assert bad[60] and bad[62]
    assert not (sure[60] or sure[61] or sure[62])
    assert sure.sum() > 0.8 * len(sure)
    for q in np.flatnonzero(sure):          # every sure piece keeps its true turn plus one constant
        assert lp[q] - T[d['pid'] == q][0] == 5


def test_clean_slice_is_all_sure():
    d, _ = ring_slice()
    sure, _, dist, bad, _ = ladders.sure_mask(d)
    assert not bad.any() and sure.all() and np.isinf(dist).all()


def test_only_the_main_component_is_written():
    d, T = ring_slice(island=150)
    doc, rec = ladders.build({10: d}, ladders.Frame())
    s = rec['slices']['10']
    assert s['components_with_sure'] == 2 and s['sure_left_out'] > 0
    pts = doc['collections']['1']['points'].values()
    assert max(p['wind_a'] for p in pts) < 30            # the island's offset never reaches the file
    with pytest.raises(ladders.LadderError):
        ladders.build({10: {k: v for k, v in d.items() if k != 'comp'}}, ladders.Frame())


def check_point_collections(doc):
    """The rules vc_object_ingest --point-collections checks
    (PointCollectionInput), plus what object_store_inputs reads."""
    assert doc['vc_pointcollections_json_version'] == '1'
    ids = []
    for col in doc['collections'].values():
        assert col['winding_is_absolute'] is False
        assert col['points']
        for key, p in col['points'].items():
            assert int(key) >= 0
            assert len(p['p']) == 3 and all(math.isfinite(v) for v in p['p'])
            assert isinstance(p['wind_a'], float)
            ids.append(int(key))
    assert len(ids) == len(set(ids))                    # unique within the document


def test_constraints_from_labelled_slices(labelled):
    for hand, R in labelled.items():
        d = as_file(R)
        doc, rec = ladders.build({100: d, 101: d}, ladders.Frame())
        check_point_collections(doc)
        assert rec['collections'] == 2
        col = doc['collections']['1']
        ids = sorted(col['points'], key=int)
        th = [np.mod(hand * math.atan2(col['points'][i]['p'][1] - CY, col['points'][i]['p'][0] - CX),
                     2 * math.pi) for i in ids]
        assert all(b >= a - 1e-6 for a, b in zip(th, th[1:]))     # numbered by theta' ascending
        assert rec['slices']['100']['max_theta_gap'] < math.pi
        w = np.array([col['points'][i]['wind_a'] for i in ids])
        x = np.array([col['points'][i]['p'][0] for i in ids])
        y = np.array([col['points'][i]['p'][1] for i in ids])
        turn = np.rint((np.hypot(x - CX, y - CY) - R0) / PITCH - np.array(th) / (2 * math.pi))
        assert w.min() == 0
        assert len(np.unique(w - turn)) == 1                      # wind_a is the true turn, one offset


def test_frame_maps_points():
    d, _ = ring_slice()
    M = [[2, 0, 0, 10], [0, 2, 0, 20], [0, 0, 2, 30]]
    a, _ = ladders.build({100: d}, ladders.Frame())
    b, _ = ladders.build({100: d}, ladders.Frame(M, 4.0))
    for u, v in zip(a['collections']['1']['points'].values(), b['collections']['1']['points'].values()):
        assert v['p'][0] == pytest.approx((2 * u['p'][0] + 10) / 4, abs=1e-3)
        assert v['p'][2] == pytest.approx((2 * u['p'][2] + 30) / 4, abs=1e-3)


def test_plus_one_control_changes_every_step():
    d, _ = ring_slice()
    doc, _ = ladders.build({7: d}, ladders.Frame())
    q = ladders.plus_one(doc)
    check_point_collections(q)
    lv = sorted({p['wind_a'] for p in doc['collections']['1']['points'].values()})
    lq = sorted({p['wind_a'] for p in q['collections']['1']['points'].values()})
    assert lv == [0, 1, 2, 3, 4, 5, 6, 7]
    assert lq == [0, 2, 4, 6, 8, 10, 12, 14]
    assert q['collections']['1']['name'].startswith('m7-winding-control-')
    assert [p['p'] for p in q['collections']['1']['points'].values()] == \
        [p['p'] for p in doc['collections']['1']['points'].values()]


# ---------------------------------------------------------------- command line

def test_command_line_end_to_end(tmp_path):
    zarr = pytest.importorskip('zarr')
    vol = np.stack([spiral_slice(-1, seed=s, gaps=40) for s in range(4)])
    pred = tmp_path / 'pred.zarr'
    group = zarr.open_group(str(pred), mode='w')     # a multiscale group, level 0 only
    group.create_array('0', shape=vol.shape, chunks=(1, 256, 256), dtype='uint8')[:] = vol
    umb = tmp_path / 'umbilicus.json'
    umb.write_text(json.dumps({'control_points': [{'z': 0, 'y': CY, 'x': CX}, {'z': 3, 'y': CY, 'x': CX}]}))
    sense = tmp_path / 'sense.json'
    assert outward_sense.main(['--prediction', str(pred), '--level', '0', '--umbilicus', str(umb),
                               '--pitch', str(PITCH), '--z-range', '0,3', '--planes', '2', '--min-votes', '2',
                               '--out', str(sense)]) == 0
    assert json.loads(sense.read_text())['call'] == 'ACW'
    params = {k: P[k] for k in ('r_min', 'track_r_max', 'track_r_lo', 'track_r_hi', 'track_rays')}
    assert label_slices.main(['--prediction', str(pred), '--level', '0', '--umbilicus', str(umb), '--z', '1', '2',
                              '--sense', str(sense), '--params', json.dumps(params),
                              '--out', str(tmp_path / 'slices')]) == 0
    assert ladders.main(['--slices', str(tmp_path / 'slices'), '--out', str(tmp_path / 'c'),
                         '--fit-scale', '4', '--control']) == 0
    doc = json.loads((tmp_path / 'c' / 'relative_windings.json').read_text())
    ctl = json.loads((tmp_path / 'c' / 'relative_windings_control.json').read_text())
    check_point_collections(doc)
    check_point_collections(ctl)
    assert [c['metadata']['z'] for c in doc['collections'].values()] == [1, 2]
    assert all(c['metadata']['hand'] == -1 for c in doc['collections'].values())
    p = next(iter(doc['collections']['1']['points'].values()))['p']
    assert p[2] == pytest.approx(0.25) and 0 < p[0] < N / 4 and 0 < p[1] < N / 4
    rec = json.loads((tmp_path / 'c' / 'ladders.json').read_text())
    assert rec['points'] == sum(len(c['points']) for c in doc['collections'].values())
