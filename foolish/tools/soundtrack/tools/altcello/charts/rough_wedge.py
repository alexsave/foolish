"""Wedge slope scan of G2 on C (roughness iteration 1): level 50 ms after the start re sustain (the test_player T27h measure) and the audio time to periodic, 6 seeds, for slopes --attk.
python -I charts/rough_wedge.py 0.667 0.69 0.71 ..."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np
import rough_lib as RL, rich_lib as R, onset_lib as L  # noqa: E402


def lev(y, t_after=0.05):
    w = 960
    a, b = int(1.0 * 48000), int(1.9 * 48000)
    e = sorted(10 * np.log10(np.mean(y[i:i + w] ** 2) + 1e-30) for i in range(a, min(b, len(y)) - w, 480))
    sus = e[len(e) // 2]
    i = int((0.5 + t_after) * 48000)
    return 10 * np.log10(np.mean(y[i:i + w] ** 2) + 1e-30) - sus


for kk in [float(a) for a in sys.argv[1:]]:
    out = []
    for s in range(6):
        y = RL.render_wav("G2c", s, "k%.3f" % kk, "", extra=("--attk", str(kk)))
        _, tp = L.t_periodic(y, R.hz(43), t_from=0.0, tag="k%.3f_%d" % (kk, s))
        out.append((lev(np.asarray(y, dtype=float)), tp))
    print("k %.3f: lev50 %s | tper %s | median tper %.2f" % (kk, " ".join("%.1f" % a for a, b in out), " ".join("%.2f" % b for a, b in out), np.median([b for a, b in out])))
