# mkmulti.py name nlayers root:dir/prefix ... : several one-shot articulations in one manifest, each at its own root note,
# so a score picks the stroke by pitch and plays it unpitched. Velocities (vN in the names) of each articulation are spread
# over nlayers layers, so every layer holds every stroke (the sampler looks inside the velocity layer first).
import sys, os, re
name, nl = sys.argv[1], int(sys.argv[2]); rows = []
for arg in sys.argv[3:]:
    root, pre = arg.split(':', 1); d, p = os.path.dirname(pre), os.path.basename(pre)
    fs = sorted(f for f in os.listdir(d) if f.startswith(p) and f.endswith('.wav'))
    if not fs: sys.exit('no files for ' + arg)
    vel = lambda f: int(re.search(r'_v(\d+)', f).group(1)) if re.search(r'_v(\d+)', f) else 1
    vs = sorted(set(vel(f) for f in fs))
    for f in fs:
        i = vs.index(vel(f)); r = re.search(r'rr(\d+)', f); rr = int(r.group(1)) if r else 1
        lo = round(i * nl / len(vs)); hi = round((i + 1) * nl / len(vs))
        for L in range(lo, max(hi, lo + 1)): rows.append(f"{root} {min(L, nl - 1)} {rr} {os.path.abspath(os.path.join(d, f))}\n")
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'samples', 'manifests', name + '.txt'), 'w').write(''.join(rows))
print(name, len(rows), 'rows')
