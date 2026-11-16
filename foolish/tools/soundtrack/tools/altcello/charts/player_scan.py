"""Scan player parameters on the single notes: python -I charts/player_scan.py  (prints HNR, time to periodic, attack per note)."""
import os, sys, itertools
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import player_run as R, player_gates as G  # noqa: E402
import numpy as np
cfgs = []
for b0 in (0.105, 0.12, 0.135):
    for vb in ((0.06, 0.07), (0.06, 0.04)):
        for fr in ((0.35, 0.25),):
            cfgs.append((b0, vb, fr))
args = sys.argv[1:]
for i, (b0, vb, fr) in enumerate(cfgs):
    tag = "scan%d" % i
    rows, msg = R.single("expert", tag, ["--vb", vb[0], vb[1], "--frac", fr[0], fr[1], "--beta0", b0])
    print("b0", b0, "vb", vb, "frac", fr, " | ".join("%d: hnr %.1f tper %s att %s lev %.1f m47 %.2f" % (m, r["hnr"], R.fmt(r["t_per"]), R.fmt(r["att10_90"]), r["lev_sd"], r["mod4_7"]) for m, r in rows.items()), flush=True)
