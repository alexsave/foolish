"""Table of out/wedge/final_*.txt (charts/wedge_final.py): per note and build the 3 seeds' time to periodic, median levels at 20/50/100 ms, tilt, HNR; Philharmonia on top.
    python -I charts/final_tab.py c3 c4 [...] """
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
def rows(label):
    d = {}
    for l in open(os.path.join(ROOT, "out", "wedge", "final_%s.txt" % label)):
        q = l.split()
        if q and q[0] == "ROW": d.setdefault(q[2], []).append([float(q[5])] + [float(x) for x in q[7:10]] + [float(q[11]), float(q[13])])
    return d
labs = ["phil"] + sys.argv[1:]
R = {l: rows(l) for l in labs}
for nm in ("G2", "D3", "A3", "C2", "G2c"):
    print("== " + nm)
    for l in labs:
        a = np.array(R[l][nm])
        print("  %-6s tper %-22s lev20/50/100 %6.1f %6.1f %6.1f  tilt %5.1f hnr %5.1f" % (l, "/".join("%.2f" % x for x in a[:, 0]), *np.median(a[:, 1:4], axis=0), np.median(a[:, 4]), np.median(a[:, 5])))
