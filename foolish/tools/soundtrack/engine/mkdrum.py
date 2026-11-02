# mkdrum.py name dir : one-shot drum samples (vlN / rrN in the names) -> samples/manifests/name.txt at root 60
import sys, os, re
name, d = sys.argv[1], sys.argv[2]
rows = []
for f in sorted(os.listdir(d)):
    if not f.endswith('.wav'): continue
    v = re.search(r'_vl?(\d+)', f); r = re.search(r'rr(\d+)', f) or re.search(r'_(\d+)\.wav$', f)
    dm = re.search(r'_(ppp|pp|p|mp|mf|f|ff|fff)(?=[_.\d])', f)
    vel = int(v.group(1)) if v else ({'ppp':0,'pp':1,'p':2,'mp':3,'mf':4,'f':5,'ff':6,'fff':7}[dm.group(1)] if dm else 1)
    rows.append((vel, int(r.group(1)) if r else 1, os.path.abspath(os.path.join(d, f))))
vels = sorted(set(x[0] for x in rows)); vm = {v: i for i, v in enumerate(vels)}
open(os.path.join(os.path.dirname(__file__), '..', 'samples', 'manifests', name + '.txt'), 'w').write(''.join(f"60 {vm[v]} {rr} {p}\n" for v, rr, p in rows))
print(name, len(rows), 'samples', len(vels), 'layers')
