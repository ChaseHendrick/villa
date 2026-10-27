"""Collect per-file facts and line geometry from the published fiber folders."""
import glob, json, os, pickle, sys
import numpy as np

FOLDERS = {'PHerc0139': 'data/0139', 'PHerc0332': 'data/0332_fibers'}
out = {}
for scroll, d in FOLDERS.items():
    files = []
    for f in sorted(glob.glob(d + '/*.json')):
        j = json.load(open(f))
        v = j.get('version')
        br = list(j.get('branches') or []) + list(j.get('adjacent_branches') or [])
        lp = np.asarray(j['line_points'], float)
        kind = 'v3' if v < 4 else ('v4_link' if br else 'v4_nolink')
        files.append(dict(
            name=os.path.basename(f), version=v, kind=kind, uuid=j.get('uuid'),
            n_branch=len(br), n_target_uuid=sum('target_uuid' in b for b in br),
            n_branch_file=sum('branch_file' in b for b in br),
            line=lp[::4].astype(np.float32),
            branch_pts=[b['control_point_position'] for b in br if 'control_point_position' in b],
            branches=br))
    out[scroll] = files
    v4 = [x for x in files if x['version'] == 4]
    print(scroll, 'files', len(files), 'v4', len(v4),
          'v4 with links', sum(x['kind'] == 'v4_link' for x in files),
          'v4 branch entries', sum(x['n_branch'] for x in v4),
          'with target_uuid', sum(x['n_target_uuid'] for x in v4),
          'with branch_file', sum(x['n_branch_file'] for x in v4),
          'v4 with uuid', sum(bool(x['uuid']) for x in v4))
pickle.dump(out, open('fig/fibers.pkl', 'wb'))
