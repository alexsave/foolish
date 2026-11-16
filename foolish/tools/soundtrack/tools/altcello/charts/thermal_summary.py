"""Tables of the thermal-vs-curve measurements from out/thermal (charts/thermal_run.sh, thermal_gates.py).

    python -I charts/thermal_summary.py [TAG ...]     TAG: the model names to compare (default: curve thermal)
Prints: contiguous Helmholtz band per case, Guettler cells at most 0.1 s (and steady cells, best time), the bow reversal times."""
import os, re, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
D = os.path.join(os.path.dirname(HERE), "out", "thermal")
BANDS = [("C2 open", "C2_0"), ("G2 on C", "C2_7"), ("D3 open", "D3_0"), ("A3 open", "A3_0")]
GRIDS = [("C2 open", "C2_0"), ("G2 on C", "C2_7"), ("D3 on G", "G2_7"), ("A3 on D", "D3_7")]


def longest(F, ok):
    best, i = (0, -1), 0
    while i < len(F):
        if ok[i]:
            j = i
            while j + 1 < len(F) and ok[j + 1]: j += 1
            if j - i + 1 > best[0]: best = (j - i + 1, i)
            i = j + 1
        else: i += 1
    if best[0] == 0: return (0, 0.0, 0.0, 0.0)
    lo, hi = F[best[1]], F[best[1] + best[0] - 1]
    return (best[0], lo, hi, hi / lo)


def band(tag, case):
    p = os.path.join(D, "band_%s_%s.txt" % (tag, case))
    if not os.path.exists(p): return None
    rows = [l.split() for l in open(p) if not l.startswith("#") and l.strip()]
    F = [float(r[0]) for r in rows]
    both = [int(r[1]) == 1 and int(r[2]) == 1 for r in rows]
    r = longest(F, both)
    return dict(n=r[0], lo=r[1], hi=r[2], ratio=r[3], npts=sum(both), nrows=len(rows))


def grid(tag, case):
    p = os.path.join(D, "grid_%s_%s.txt" % (tag, case))
    if not os.path.exists(p): return None
    rows = [l.split() for l in open(p) if not l.startswith("#") and l.strip()]
    ta = [float(r[2]) for r in rows]; st = [int(r[6]) for r in rows]
    ok = [i for i in range(len(rows)) if st[i] == 1 and ta[i] >= 0]
    fast = [i for i in ok if ta[i] <= 0.1]
    return dict(cells=len(rows), fast=len(fast), fast80=sum(ta[i] <= 0.08 for i in ok), steady=sum(st), best=min([ta[i] for i in ok], default=-1.0))


def rev(tag):
    out = {}
    for f in sorted(os.listdir(D)):
        if not f.startswith("rev_") or not f.endswith(".txt"): continue
        for l in open(os.path.join(D, f)):
            m = re.match(r"(\S+) (\S+) seed (\d+): .*pre_ok (\d+).*t_reper (\S+)", l)
            if m and m.group(1) == tag: out.setdefault(m.group(2), []).append(float(m.group(5)))
    return out or None


LABELS = [("36", "C2"), ("43c", "G2 on C"), ("43", "G2"), ("50", "D3"), ("57", "A3")]
GKEYS = ["tper", "att10_90", "att_tilt", "hnr", "share13"]


def gates(tag):
    p = os.path.join(D, "gates_%s.txt" % tag)
    if not os.path.exists(p): return None
    out = {}
    for l in open(p):
        if l.startswith("ROW") and l.split()[1] == tag:
            q = l.split(); out.setdefault(q[2], []).append([float(x) for x in q[4:]])
    return out


def gate_table(tags):
    ph = gates("curve") and {k: v for k, v in gates_phil().items()}
    for k, nm in GKEYS_PRINT:
        print("  %s" % nm)
        for lab, sn in LABELS:
            cells = []
            for t in tags:
                g = gates(t)
                if g and lab in g: cells.append("%s %s" % (t, " ".join("%.2f" % x[GKEYS.index(k)] for x in g[lab])) + " (med %.2f)" % np.median([x[GKEYS.index(k)] for x in g[lab]]))
            if ph and lab in ph: cells.append("phil %.2f" % ph[lab][0][GKEYS.index(k)])
            print("    %-8s " % sn + " | ".join(cells))


def gates_phil():
    p = os.path.join(D, "gates_curve.txt")
    out = {}
    for l in open(p):
        if l.startswith("ROW phil"):
            q = l.split(); out.setdefault(q[2], []).append([float(x) for x in q[4:]])
    return out


GKEYS_PRINT = [("tper", "audio time to periodic, s (3 seeds)"), ("hnr", "HNR, dB"), ("share13", "1-3 kHz share of 50-16000 Hz, dB"), ("att_tilt", "2-6 kHz attack minus sustain, dB")]


def main():
    tags = sys.argv[1:] or ["curve", "thermal"]
    print("BAND (beta 0.1, v_B 0.1, 32 forces 0.1-8 N): contiguous run, both classifiers")
    for lab, c in BANDS:
        print("  %-8s " % lab + " | ".join("%s: %s" % (t, ("x%.2f (%d pts, %.3f-%.3f N; %d of %d Helmholtz)" % (b["ratio"], b["n"], b["lo"], b["hi"], b["npts"], b["nrows"])) if (b := band(t, c)) and b["n"] else "none" if band(t, c) else "-") for t in tags))
    print("GUETTLER (72 cells): cells at steady Helmholtz with audio time to periodic <= 0.1 s [<= 0.08] / steady cells / best")
    for lab, c in GRIDS:
        print("  %-8s " % lab + " | ".join("%s: %s" % (t, ("%d [%d] / %d / %.3f s" % (g["fast"], g["fast80"], g["steady"], g["best"])) if (g := grid(t, c)) else "-") for t in tags))
    print("EXPERT NOTES")
    gate_table(tags)
    print("REVERSAL (t_reper after the start of the reversal, s, 3 seeds, -1 = never)")
    for t in tags:
        r = rev(t)
        if r: print("  %-10s " % t + "  ".join("%s %s median %.3f" % (k, ["%.3f" % x for x in v], float(np.median([x if x >= 0 else 9.9 for x in v]))) for k, v in sorted(r.items())))


if __name__ == "__main__":
    main()
