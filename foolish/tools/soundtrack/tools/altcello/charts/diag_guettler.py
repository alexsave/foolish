"""Guettler sweep: force first (5 ms), then a constant bow acceleration a (v_B from 0 to 0.1 m/s in 0.1/a seconds), a grid of (F, a) per string.
Prints the time to 10 consecutive one-slip periods (lenient; 'x' = never) and the best cell.

    diag_guettler.py TAG KNOBS... -- (the knobs are probe options, e.g. the C1 body)   -> out/diag/guettler_TAG.txt
"""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import diag_render as R, diagnose as D  # noqa: E402
MID = {"C2": 2.36, "G2": 1.29, "D3": 1.29, "A3": 0.873}  # middle of the C1 band, N (diag_band.py, c1)

def main():
    tag = sys.argv[1]
    knobs = [float(k) if k.replace(".", "").replace("-", "").isdigit() else k for k in sys.argv[2:]]
    out = open(os.path.join(R.OUTD, "guettler_%s.txt" % tag), "w")
    accs = [0.5, 1.0, 2.0, 4.0, 8.0, 16.0]
    for s in ["C2", "G2", "D3", "A3"]:
        fs = MID[s] * np.array([0.6, 0.8, 1.0, 1.25, 1.6])
        out.write("%s  rows F (N), columns a (m/s2) %s\n" % (s, accs))
        best = (9.0, None)
        for F in fs:
            line = []
            for a in accs:
                T = 0.1 / a
                kn = ["--fb-env", "0:0,0.005:%g" % F, "--vb-env", "0:0,%g:0.1" % T] + knobs
                w, pre = R.render(s, "gu_", F=float(F), secs=1.2, knobs=kn, wav_on=False, string=s)
                t = D.slip_stats(pre, R.OPEN[s])["lenient"][0]
                for e in (".f32", "_snap.f32", ".txt", "_snap.txt"):
                    try: os.remove(pre + e)
                    except OSError: pass
                line.append(t)
                if t == t and t < best[0]: best = (t, (F, a))
            out.write("%.3f " % F + " ".join("x" if t != t else "%.3f" % t for t in line) + "\n")
        out.write("best %s %s\n" % (s, best)); print(tag, s, "best", best, flush=True)
    out.close()

main()
