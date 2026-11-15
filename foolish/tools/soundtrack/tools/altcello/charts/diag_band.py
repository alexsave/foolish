"""Playable band: sweep F_B at beta 0.1, v_B 0.1 on an open string, classify each force by the simulator's slip pattern.

    diag_band.py TAG STRING [probe knobs ...]   -> out/diag/band_TAG_STRING.txt and a line  F_min F_max ratio  (Helmholtz = at least 90 percent of
    the periods after 1.5 s have exactly one slip, and 10 consecutive one-slip periods occur within the 2.5 s)
"""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import diag_render as R, diagnose as D  # noqa: E402

UP = {"C2": "G2", "G2": "D3", "D3": "A3"}


def sweep(tag, string, knobs=(), beta=0.1, grid=None, stopped=False):
    grid = grid or np.geomspace(0.15, 3.0, 17) * R.ANCHOR[string] / 0.67
    rows = []
    for F in grid:
        if stopped:
            w, pre = R.render(UP[string], "band_%s_%.4f_" % (tag, F), beta=beta, F=float(F), secs=2.5, knobs=list(knobs), wav_on=False, stopped=True)
            f0 = R.OPEN[UP[string]]
        else:
            w, pre = R.render(string, "band_%s_%.4f_" % (tag, F), beta=beta, F=float(F), secs=2.5, knobs=list(knobs), wav_on=False, string=string)
            f0 = R.OPEN[string]
        ss = D.slip_stats(pre, f0, late_from=1.5)["lenient"]
        for e in (".f32", "_snap.f32", ".txt", "_snap.txt"): 
            try: os.remove(pre + e)
            except OSError: pass
        rows.append((float(F), ss[0], ss[1]))
    ok = [r[0] for r in rows if r[2] >= 0.9 and r[1] == r[1]]
    return rows, (min(ok), max(ok), max(ok) / min(ok)) if ok else None

if __name__ == "__main__":
    tag, string = sys.argv[1], sys.argv[2]
    stopped = "--stopped" in sys.argv
    knobs = [float(k) if k.replace(".", "").replace("-", "").replace("e", "").isdigit() else k for k in sys.argv[3:] if k != "--stopped"]
    rows, band = sweep(tag, string, knobs, stopped=stopped)
    with open(os.path.join(R.OUTD, "band_%s_%s%s.txt" % (tag, string, "_stopped" if stopped else "")), "w") as f:
        for r in rows: f.write("%.4f %.3f %.2f\n" % r)
    print(tag, string, "band", band, flush=True)
