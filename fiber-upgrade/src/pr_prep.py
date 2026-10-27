"""Read the fibers and branch edges back out of the stores built from the upgraded folders."""
import json, pickle
import numpy as np
import vc_object_store as vos

out = {}
for scroll, path in (('PHerc0139', 'fig/up0139.vcobj'), ('PHerc0332', 'fig/up0332.vcobj')):
    store = vos.ObjectStore.open(path, read_only=True)
    lines = []
    for record in store.objects_by_kind('fiber'):
        revision = store.current_geometry(record['uuid'])['uuid']
        points = np.frombuffer(store.read_geometry_array(revision, 'line_points'), '<f8').reshape(-1, 3)
        lines.append(points[::4].astype(np.float32))
    edges = [(e['a_anchor']['position'], e['b_anchor']['position']) for e in store.edges_by_kind('fiber_branch')]
    out[scroll] = dict(lines=lines, edges=np.asarray(edges, float))
    print(scroll, len(lines), 'fibers', len(edges), 'edges')
res = json.load(open('fig/ingest-results.json'))
pickle.dump(out, open('fig/store.pkl', 'wb'))
