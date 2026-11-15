"""Philharmonia gate distribution over all its cello notes (out/diag/refgates.tsv)."""
import os, sys, glob
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diagnose as D
out = os.path.join(D.ROOT, "out", "diag", "refgates.tsv")
rows = []
for m in range(38, 68):
    path = D.PHIL % m
    if not os.path.exists(path): continue
    nt = D.Note(path, D.M.midi_hz(m))
    g = D.gates(nt)
    rows.append((m, g))
    print(m, {k: round(float(v), 3) for k, v in g.items()}, flush=True)
keys = list(rows[0][1].keys())
with open(out, "w") as f:
    f.write("midi " + " ".join(keys) + "\n")
    for m, g in rows: f.write("%d " % m + " ".join("%.5g" % g[k] for k in keys) + "\n")
