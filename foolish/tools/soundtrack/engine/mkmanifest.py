# mkmanifest.py name dir [dir...] : index sample WAVs by note/layer/round-robin from their file names -> samples/manifests/name.txt
# (reads names only; the samples are data)
import sys, os, re
name, dirs = sys.argv[1], sys.argv[2:]
NOTE = re.compile(r'(?:^|[_\- ])([A-Ga-g])([#b]?)(-?\d)(?=[_\- .v])')
VEL = re.compile(r'(?:_|^)(?:v|dyn|vl)(\d+)', re.I)
DYN = {'ppp':0,'pp':1,'p':2,'mp':3,'mf':4,'f':5,'ff':6,'fff':7}
DYNRE = re.compile(r'_(ppp|pp|p|mp|mf|f|ff|fff)(?=[_.])')
RR = re.compile(r'(?:rr|RR|_)(\d+)(?:_Sum)?\.wav$')
rows = []
for d in dirs:
    for root, _, fs in os.walk(d):
        for f in sorted(fs):
            if not f.lower().endswith('.wav'): continue
            m = NOTE.search(f)
            if not m: continue
            pc = {'C':0,'D':2,'E':4,'F':5,'G':7,'A':9,'B':11}[m.group(1).upper()] + (1 if m.group(2)=='#' else -1 if m.group(2)=='b' else 0)
            midi = pc + 12*(int(m.group(3))+1)
            v = VEL.search(f); dm = DYNRE.search(f)
            vel = int(v.group(1)) if v else (DYN[dm.group(1)] if dm else 1)
            r = RR.search(f); rr = int(r.group(1)) if r else 1
            rows.append((midi, vel, rr, os.path.abspath(os.path.join(root, f))))
vels = sorted(set(r[1] for r in rows)); vmap = {v:i for i,v in enumerate(vels)}
out = os.path.join(os.path.dirname(__file__), '..', 'samples', 'manifests', name + '.txt')
with open(out, 'w') as o:
    for midi, vel, rr, p in sorted(rows): o.write(f"{midi} {vmap[vel]} {rr} {p}\n")
print(name, len(rows), 'samples', 'notes', min(r[0] for r in rows) if rows else '-', max(r[0] for r in rows) if rows else '-', 'layers', len(vels))
