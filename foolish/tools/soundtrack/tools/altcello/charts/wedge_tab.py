"""Summarise a wedge scan (charts/wedge_scan.py): per config and note the median over seeds of time to periodic and levels, the worst tper.
    python -I charts/wedge_tab.py scan_NAME [more]"""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
PH = {43: (-8.1, -6.8, -3.7), 50: (-3.7, -3.4, -3.6), 57: (-5.2, -2.6, -0.8)}
NM = {43: "G2/C", 50: "D3/G", 57: "A3/D"}
for name in sys.argv[1:]:
    rows = {}
    for l in open(os.path.join(ROOT, "out", "wedge", name + ".txt")):
        q = l.split()
        if not q or q[0] != "ROW": continue
        rows.setdefault((q[1], int(q[2])), []).append((int(q[3]), float(q[5]), float(q[7]), float(q[8]), float(q[9]), float(q[11]), float(q[13])))
    cfgs = []
    for (c, m) in rows:
        if c not in cfgs: cfgs.append(c)
    print("== " + name + "   (tper s: seeds; lev20/50/100 dB median; Philharmonia G2 -8.1/-6.8/-3.7 D3 -3.7/-3.4/-3.6 A3 -5.2/-2.6/-0.8)")
    for c in cfgs:
        line = "%-10s" % c
        for m in (43, 50, 57):
            r = rows[(c, m)]
            tp = [x[1] for x in r]
            lv = np.median([x[2:5] for x in r], axis=0)
            line += " | %s tper %s lev %5.1f %5.1f %5.1f tilt %4.1f hnr %4.1f" % (NM[m], "/".join("%.2f" % t for t in tp), lv[0], lv[1], lv[2], np.median([x[5] for x in r]), np.median([x[6] for x in r]))
        print(line)
