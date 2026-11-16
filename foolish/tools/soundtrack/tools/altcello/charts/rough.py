"""Low-note roughness chart (iteration 1): slip regularity, bow noise, contact width, torsion.   python -I charts/rough.py [OUT.png]
Data: out/rough/res_TAG.pkl (audio, 3 seeds) and the cached contact traces out/rough/TAG_<note>_s<seed>.f32 (rough_tool; see rough_lib.py)."""
import os, sys, pickle
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np
import rough_lib as RL, rich_lib as R  # noqa: E402
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

OUTP = sys.argv[1] if len(sys.argv) > 1 else "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/rough_iter1.png"
FS = 96000
REAL = {"D2": 18.7, "G2c": 25.4, "D3": 25.6, "A3": 22.8}
# (tag, label) in the order of the factor table
CFG = [("base", "cand. 5 (lumped, shipped)"),
       ("f1_0.5", "F1 noise C x0.5"), ("f1_0.25", "F1 noise C x0.25"), ("f1_0", "F1 noise C off"),
       ("f2_0.005", "F2 hair 5 mm"), ("f2_0.015", "F2 hair 15 mm"), ("f2_0.02", "F2 hair 20 mm"),
       ("f3_n2", "F3 2 points"), ("f3_n3", "F3 3 points"), ("c_n4", "F3 4 points (adopted)"), ("f3_n3t", "F3 3 pts tilt 0.5"),
       ("f4_z1", "F4 zeta_t 0.1"), ("f4_zt5", "F4 Z_t 5 Z0"), ("f4_both", "F4 both"),
       ("f5_frac0.3", "F5 force 30%"), ("f5_frac0.4", "F5 force 40%"), ("f5_frac0.5", "F5 force 50%"), ("f5_frac0.65", "F5 force 65%"), ("f5_frac0.8", "F5 force 80%"),
       ("f5_beta0.101", "F5 beta ~0.08"), ("f5_beta0.121", "F5 beta ~0.10"), ("f5_beta0.141", "F5 beta ~0.12"),
       ("f5_vb0.08", "F5 speed 0.08"), ("f5_vb0.10", "F5 speed 0.10"), ("f5_vb0.14", "F5 speed 0.14"),
       ("f6_v5", "F6 vibrato 5 c"),
       ("c_n4hp1", "4 pts + noise HP 1 kHz"), ("c_n4s5", "4 pts + noise C x0.5"), ("c_n4b115", "4 pts + beta +0.01"), ("c_n4vb10", "4 pts + speed 0.10"), ("c_n4zt5", "4 pts + Z_t 5 Z0")]


def stats(tag, k):
    pk = os.path.join(RL.OUT, "res_%s.pkl" % tag)
    if not os.path.exists(pk): return None
    res = pickle.load(open(pk, "rb"))
    if k not in res: return None
    rows = res[k]
    t = [RL.trace_stats(RL.run_trace(k, i, tag), R.hz(R.NOTES[k][0])) for i in range(len(rows))]
    return dict(hnr=RL.mean(rows, "hnr"), hs=[r["hnr"] for r in rows], tper=float(np.median([r["tper"] for r in rows])), pm=float(np.nanmean([x["pm"] for x in t])), ex=float(np.nanmean([x["extra"] for x in t])),
                sj=float(np.nanmean([x["jit"] for x in t])), dsd=float(np.nanmean([x["dsd"] for x in t])))


def vrel_window(tag, k, seed, nper=2.0, t0=1.2):
    tr = RL.run_trace(k, seed, tag)
    v = tr["vrel"]
    T0 = FS / R.hz(R.NOTES[k][0])
    a = int(t0 * FS)
    s, e = RL.runs(np.abs(v[a:a + 4 * int(T0)]) > RL.THR)
    i0 = a + (s[0] if len(s) else 0) - int(0.12 * T0)
    n = int(nper * T0 + 0.24 * T0)
    return (np.arange(n) / FS * 1e3), v[i0:i0 + n]


fig = plt.figure(figsize=(16, 11), dpi=100)
fig.suptitle("ALT-CELLO - low-note roughness: slip regularity, bow noise, contact width, torsion - iteration 1", fontsize=17, fontweight="bold", y=0.985)
gs = fig.add_gridspec(3, 4, left=0.055, right=0.985, top=0.925, bottom=0.06, hspace=0.42, wspace=0.28, height_ratios=[1.0, 1.15, 1.25])
# A: v_rel over two periods
cols = {"base": "#c0392b", "c_n4": "#1f6fb4"}
for j, (k, ttl) in enumerate([("G2c", "G2 on C"), ("D3", "D3 on G")]):
    for i, (tag, lab) in enumerate([("base", "before: lumped contact"), ("c_n4", "after: 4 contact points")]):
        ax = fig.add_subplot(gs[0, 2 * j + i])
        t, v = vrel_window(tag, k, 0)
        ax.plot(t, v, color=cols[tag], lw=0.9)
        ax.axhline(0, color="#888", lw=0.5)
        ax.set_title("%s - %s" % (ttl, lab), fontsize=10.5)
        ax.set_xlabel("ms (2 periods, seed 0)", fontsize=9)
        if i == 0 and j == 0: ax.set_ylabel("v_rel at the bow, m/s", fontsize=9)
        ax.set_ylim(-1.9, 0.3)
        st = stats(tag, k)
        ax.text(0.98, 0.04, "extra-slip periods %.0f%%\nHNR %.1f dB" % (100 * st["pm"], st["hnr"]), transform=ax.transAxes, ha="right", va="bottom", fontsize=9, bbox=dict(fc="white", ec="#bbb", alpha=0.85))
        ax.tick_params(labelsize=8)
# B: share of periods with an extra slip per factor (D2, G2c), C: HNR per factor
labels = [l for t, l in CFG]
yy = np.arange(len(CFG))
D = {k: [stats(t, k) for t, l in CFG] for k in ("D2", "G2c")}
axB = fig.add_subplot(gs[1:, 0:2])
axC = fig.add_subplot(gs[1:, 2:4], sharey=axB)
h = 0.38
for ax, key, ttl, xl in ((axB, "pm", "Share of periods with an extra slip (0 = Helmholtz, 1 = steady double slip, 0.5 = switching)", "share of periods"), (axC, "hnr", "HNR of the sustained note (3 seeds)", "dB")):
    for off, k, c in ((-h / 2, "D2", "#2a9d8f"), (h / 2, "G2c", "#e07b00")):
        vals = [(d[key] if d else np.nan) for d in D[k]]
        ax.barh(yy + off, vals, height=h, color=c, label={"D2": "D2 (C2+2 on C)", "G2c": "G2 on C"}[k], alpha=0.95)
        if key == "hnr":
            for yi, d in zip(yy, D[k]):
                if d:
                    ax.plot(d["hs"], [yi + off] * len(d["hs"]), "k.", ms=2.5)
    ax.set_title(ttl, fontsize=10.5)
    ax.set_xlabel(xl, fontsize=9)
    ax.invert_yaxis()
    ax.tick_params(labelsize=8)
    ax.grid(axis="x", color="#ddd", lw=0.6)
    ax.set_axisbelow(True)
axB.set_yticks(yy)
axB.set_yticklabels(labels, fontsize=8)
plt.setp(axC.get_yticklabels(), visible=False)
axC.axvline(REAL["D2"], color="#2a9d8f", lw=1.6, ls="--")
axC.axvline(REAL["G2c"], color="#e07b00", lw=1.6, ls="--")
axC.text(REAL["D2"] - 0.2, len(CFG) - 0.2, "real D2 %.1f" % REAL["D2"], color="#2a9d8f", fontsize=8.5, ha="right", va="bottom")
axC.text(REAL["G2c"] - 0.2, len(CFG) - 0.2, "real G2 %.1f" % REAL["G2c"], color="#e07b00", fontsize=8.5, ha="right", va="bottom")
axC.set_xlim(0, 28)
axB.legend(loc="lower right", fontsize=9, frameon=True)
b6 = {k: stats("c_n4", k) for k in ("D2", "G2c", "D3", "A3")}
b5 = {k: stats("base", k) for k in ("D2", "G2c", "D3", "A3")}
msg = ("HNR  cand 5 -> cand 6 (real):  " + "   ".join("%s %.1f -> %.1f (%.1f)" % (k, b5[k]["hnr"], b6[k]["hnr"], REAL[k]) for k in ("D2", "G2c", "D3", "A3")) +
       "\ntime to periodic (s):  " + "   ".join("%s %.2f -> %.2f" % (k, b5[k]["tper"], b6[k]["tper"]) for k in ("D2", "G2c", "D3", "A3")) +
       "\nextra-slip periods:  " + "   ".join("%s %.0f%% -> %.0f%%" % (k, 100 * b5[k]["pm"], 100 * b6[k]["pm"]) for k in ("D2", "G2c", "D3", "A3")))
fig.text(0.055, 0.012, msg, fontsize=9.5, family="monospace", va="bottom")
fig.savefig(OUTP, dpi=100)
print(OUTP)
