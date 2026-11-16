"""One line per configuration: python -I charts/rough_tab.py TAG [TAG ...]  (audio from out/rough/res_TAG.pkl, the slip pattern recomputed from the cached traces)
columns per note: HNR dB | tper s | pm = share of periods with an extra slip (0 = Helmholtz, 1 = a steady double slip, 0.5 = switching) | extra slips per period | slip size sd | slip jitter % | audio jitter %"""
import os, sys, pickle
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np
import rough_lib as RL, rich_lib as R  # noqa: E402
print("%-16s " % "config" + " ".join("%-62s" % k for k in ("D2", "G2c", "D3", "A3")))
for tag in sys.argv[1:]:
    pk = os.path.join(RL.OUT, "res_%s.pkl" % tag)
    if not os.path.exists(pk): print(tag, "missing"); continue
    res = pickle.load(open(pk, "rb"))
    cells = []
    for k in ("D2", "G2c", "D3", "A3"):
        if k not in res: cells.append(" " * 62); continue
        rows = res[k]
        t = [RL.trace_stats(RL.run_trace(k, i, tag), R.hz(R.NOTES[k][0])) for i in range(len(rows))]
        cells.append("%5.1f %4.2f pm %.2f ex %.2f sj %.2f aj %.2f gap %+.1f %+.1f" % (RL.mean(rows, "hnr"), float(np.median([r["tper"] for r in rows])), np.nanmean([x.get("pm", np.nan) for x in t]),
                     np.nanmean([x["extra"] for x in t]), np.nanmean([x["jit"] for x in t]), RL.mean(rows, "jit"),
                     np.nanmean([r["gap"][2] for r in rows]) if "gap" in rows[0] else np.nan, np.nanmean([r["gap"][3] for r in rows]) if "gap" in rows[0] else np.nan))
    print("%-16s " % tag + " ".join("%-62s" % c for c in cells))
