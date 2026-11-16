"""Summary of the wedge Guettler maps (charts/wedge_run.sh): per string the table F x a of time to periodic (s) and level re sustain at 50 ms, the cells that are
periodic within 0.08 s (steady) and within 4 dB of the Philharmonia 50 ms level, and the wedge slope F/a fitted through them.
    python -I charts/wedge_map.py [MAPDIR] -> stdout"""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TARGET50 = {"G2onC": -6.8, "G2open": -6.8, "D3onG": -3.4, "A3onD": -2.6, "A3open": -2.6, "C2open": -4.0}  # Philharmonia G2, D3, A3 level re sustain at +50 ms (attack_scan2.txt); C2: no Philharmonia C2, -4 assumed (no pass uses it)


def load(path):
    rows = []
    for l in open(path):
        if l.startswith("#"): continue
        q = l.split()
        rows.append([float(x) for x in q])
    return np.array(rows)


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "out", "wedge")
    for name, tgt in TARGET50.items():
        p = os.path.join(d, "map_%s.txt" % name)
        if not os.path.exists(p): continue
        R = load(p)
        Fs = sorted(set(R[:, 0])); As = sorted(set(R[:, 1]))
        print("== %s (target level at 50 ms %.1f dB; cell = t_per s / lev50 dB; * = periodic <= 0.08 s and within 4 dB; steady flag s)" % (name, tgt))
        print("%8s " % "F\\a" + " ".join("%14g" % a for a in As))
        good = []
        for F in Fs:
            cells = []
            for a in As:
                r = R[(R[:, 0] == F) & (R[:, 1] == a)][0]
                ok = r[2] >= 0 and r[2] <= 0.08 and r[6] > 0 and abs(r[8] - tgt) <= 4.0
                if ok: good.append((F, a, r[2], r[8]))
                cells.append("%s%5s/%6.1f%s" % ("*" if ok else " ", "%.3f" % r[2] if r[2] >= 0 else "never", r[8], "s" if r[6] > 0 else " "))
            print("%8.3f " % F + " ".join(cells))
        print("good cells: %d" % len(good))
        for g in good: print("   F %.3f a %g F/a %.3f t_per %.3f lev50 %.1f" % (g[0], g[1], g[0] / g[1], g[2], g[3]))
main()
