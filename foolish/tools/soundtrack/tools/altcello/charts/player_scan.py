"""Compare player parameter sets on 9 forte notes (G2 D3 A3, seeds 0 1 2): python -I charts/player_scan.py   -> medians of the gates."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import player_run as R, player_gates as G  # noqa: E402
import numpy as np
SETS = {
    "A": ["--beta0", 0.105, "--vb", 0.06, 0.12, "--acc", 0.06, 0.12],
    "B": ["--beta0", 0.12, "--vb", 0.06, 0.07, "--acc", 0.09, 0.12],
    "C": ["--beta0", 0.12, "--vb", 0.06, 0.12, "--acc", 0.06, 0.12],
    "D": ["--beta0", 0.105, "--vb", 0.06, 0.07, "--acc", 0.06, 0.12],
    "E": ["--beta0", 0.12, "--vb", 0.06, 0.04, "--acc", 0.06, 0.12],
}
names = sys.argv[1:] or list(SETS)
for nm in names:
    allr = []
    for seed in (0, 1, 2):
        rows, _ = R.single("expert", "scan_%s_%d" % (nm, seed), SETS[nm] + ["--seed", seed])
        allr += list(rows.values())
    med = G.median_table(allr)
    bad = sum(1 for r in allr if r["hnr"] < 18)
    print(nm, SETS[nm], "| n=%d bad(hnr<18)=%d | " % (len(allr), bad) + " ".join("%s %.3g" % (k, med[k]) for k in ("t_per", "att10_90", "att_tilt", "mod4_7", "lev_sd", "hnr", "vib_depth", "vib_reg_m")), flush=True)
