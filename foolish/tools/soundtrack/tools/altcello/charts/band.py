"""Playability chart of iteration 2 -> /Users/alex/Music/foolish_cities_v5/alt_cello/charts/playability_iter2.png

    /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/band.py
Reads the sweep rows of charts/iter2_run.sh (iter2/*.txt: F ours simple ...) and iter2/gate.tsv (charts/iter2_gate.py).
"""
import os, glob
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.gridspec import GridSpec
from matplotlib.lines import Line2D

D = "/Users/alex/Music/foolish_cities_v5/alt_cello/iter2"
OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/playability_iter2.png"


def rows(name):
    p = os.path.join(D, name + ".txt")
    if not os.path.exists(p): return None
    r = [ln.split() for ln in open(p) if not ln.startswith("#") and ln.strip()]
    return np.array([float(x[0]) for x in r]), np.array([int(x[1]) for x in r]), np.array([int(x[2]) for x in r])


def run(F, ok):
    best = (0, 0); i = 0
    while i < len(F):
        if ok[i]:
            j = i
            while j + 1 < len(F) and ok[j + 1]: j += 1
            if j - i + 1 > best[0]: best = (j - i + 1, i)
            i = j + 1
        else: i += 1
    if best[0] == 0: return None
    return F[best[1]], F[best[1] + best[0] - 1]


def ratio(name, which=1):
    r = rows(name)
    if r is None: return None
    b = run(r[0], r[which])
    return b


BLUE, ORANGE, GREY, RED = "#2a6fb0", "#d9822b", "#8a8a8a", "#c0392b"
plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False})
fig = plt.figure(figsize=(16, 11), dpi=100)
fig.suptitle("ALT-CELLO - playable bow-force band after the noise and torsion fixes - iteration 2", fontsize=19, fontweight="bold", y=0.985)
gs = GridSpec(3, 4, figure=fig, left=0.055, right=0.985, top=0.925, bottom=0.04, hspace=0.62, wspace=0.28, height_ratios=[1.15, 1.7, 1.0])

# A: band ratio vs noise level, D3, two torsion settings
ax = fig.add_subplot(gs[0, 0:2])
lv = [0, 0.005, 0.01, 0.02, 0.03]
for lab, col, pat, ls in (("zeta_t 0.01 (iteration 1 torsion)", GREY, "noise_D3_%g", "o--"), ("zeta_t 0.05 (chosen)", BLUE, "noise2_D3_%g", "o-")):
    xs, ys = [], []
    for n in lv:
        nm = (pat % n) if not (pat.startswith("noise2") and n == 0.01) else "zeta_D3_0.05"
        b = ratio(nm)
        xs.append(lv.index(n)); ys.append(b[1] / b[0] if b else np.nan)
    ax.plot(xs, ys, ls, color=col, label=lab, lw=2)
ax.axhline(4, color=RED, ls=":", lw=1.5); ax.text(-0.2, 4.15, "goal x4", color=RED, fontsize=9)
ax.set_xticks(range(5)); ax.set_xticklabels(["0", "0.005", "0.01", "0.02", "0.03"])
ax.axvline(2, color=ORANGE, lw=10, alpha=0.25)
ax.set_xlabel("bow noise level (std of raw draws, on the sliding friction coefficient)"); ax.set_ylabel("longest contiguous band F_max / F_min")
ax.set_title("D3 band ratio vs bow-noise level (beta 0.1, v_B 0.1, 32 forces; shaded = chosen)", fontsize=10.5, loc="left")
ax.legend(frameon=False, loc="upper left"); ax.set_ylim(0, 7)

# B: band vs torsion parameters (floating bars)
ax = fig.add_subplot(gs[0, 2:4])
cfg = [("Z_t/Z0 2", "zt_D3_2"), ("Z_t/Z0 3", "tors_D3_base_zt3_ct5_zeta0.01"), ("Z_t/Z0 5", "zt_D3_5"), ("Z_t/Z0 8", "zt_D3_8"),
       ("c_t/c 3", "ct_D3_3"), ("c_t/c 5", "tors_D3_base_zt3_ct5_zeta0.01"), ("c_t/c 7", "ct_D3_7"),
       ("zeta 0.01", "tors_D3_base_zt3_ct5_zeta0.01"), ("zeta 0.03", "zeta_D3_0.03"), ("zeta 0.05", "zeta_D3_0.05")]
for i, (lab, nm) in enumerate(cfg):
    b = ratio(nm)
    col = BLUE if "zeta 0.05" in lab else GREY
    if b:
        ax.bar(i, b[1] - b[0], bottom=b[0], color=col, width=0.7)
        ax.text(i, b[1] * 1.08, "x%.1f" % (b[1] / b[0]), ha="center", fontsize=9)
    else:
        ax.text(i, 0.06, "no band", ha="center", fontsize=9, color=RED)
ax.set_yscale("log"); ax.set_ylim(0.04, 4); ax.set_xticks(range(len(cfg))); ax.set_xticklabels([c[0] for c in cfg], rotation=35, ha="right")
ax.set_ylabel("contiguous Helmholtz band (N)")
ax.set_title("D3 band vs each torsion parameter (bars F_min..F_max, label = ratio)", fontsize=10.5, loc="left")

# C: Schelleng maps
STR = ["C2", "G2", "D3", "A3"]; BET = [0.07, 0.1, 0.14]; VB = [0.05, 0.1, 0.2]
def tg(x): return "0.1" if x == 0.1 else "%g" % x
sub = gs[1, :].subgridspec(3, 4, hspace=0.25, wspace=0.12)
for bi, b in enumerate(BET):
    for si, s in enumerate(STR):
        ax = fig.add_subplot(sub[bi, si])
        for vi, v in enumerate(VB):
            r = rows("grid_%s_b%s_v%s" % (s, tg(b), tg(v)))
            if r is None: continue
            F, o, sm = r
            both = (o == 1) & (sm == 1); one = ((o == 1) ^ (sm == 1))
            none = ~(both | one)
            ax.plot(F[none], np.full(none.sum(), vi), "x", color=GREY, ms=4, mew=1)
            ax.plot(F[one], np.full(one.sum(), vi), "o", mfc="none", mec=ORANGE, ms=6)
            ax.plot(F[both], np.full(both.sum(), vi), "o", color=BLUE, ms=6)
            bb = run(F, both)
            if bb: ax.plot([bb[0], bb[1]], [vi + 0.28] * 2, "-", color=BLUE, lw=2)
        ax.set_xscale("log"); ax.set_xlim(0.025, 6); ax.set_ylim(-0.5, 2.6)
        ax.set_yticks(range(3)); ax.set_yticklabels(["%g" % v for v in VB] if si == 0 else [])
        if bi < 2: ax.set_xticklabels([])
        if si == 0: ax.set_ylabel("beta %.2f\nv_B (m/s)" % b)
        if bi == 0: ax.set_title(s, fontsize=11, fontweight="bold")
        if bi == 2: ax.set_xlabel("bow force (N)")
fig.text(0.055, 0.665, "Schelleng maps at the new defaults (20 forces 0.03-5 N, 6 s, last 0.5 s classified; line = longest contiguous run accepted by both classifiers, the playability table)", fontsize=10.5)
leg = [Line2D([], [], marker="o", color=BLUE, ls="", label="Helmholtz, both classifiers"), Line2D([], [], marker="o", mfc="none", mec=ORANGE, ls="", label="one classifier only"),
       Line2D([], [], marker="x", color=GREY, ls="", label="not Helmholtz"), Line2D([], [], color=BLUE, lw=2, label="table band (contiguous)")]
fig.legend(handles=leg, loc="upper right", bbox_to_anchor=(0.985, 0.672), ncol=4, frameon=False, fontsize=9.5)

# D: gate table
ax = fig.add_subplot(gs[2, :]); ax.axis("off"); ax.set_position([0.055, 0.0, 0.93, 0.225])
g = os.path.join(D, "gate.tsv")
cols = ["jit %", "shim dB", "nf 0-1k", "nf 1-2.5k", "nf 2.5-5k", "nf 5-10k", "HNR", "cent/f0", "tr dB", "t_slip s", "unm. bands", "1-3k", "3-8k", "mod 4-7"]
idx = [0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15]
if os.path.exists(g):
    cell = []; colr = []
    names = {"G2": "G2 open", "D3": "D3 on G + vib", "A3": "A3 on D + vib"}
    for ln in open(g):
        t = ln.split(); p, kind = t[0], t[1]; v = [float(x) for x in t[2:]]
        lab = {"NEW": "new defaults", "C1": "C1 (iter 1)", "REF": "Philharmonia"}[kind]
        cell.append([names[p], lab] + [("n/a" if x != x else ("%.2f" % x if i in (0, 1, 15) else "%.1f" % x)) for i, x in ((i, v[i]) for i in idx)])
        colr.append(["#e3eefb" if kind == "NEW" else ("#f1f1f1" if kind == "REF" else "white")] * (len(idx) + 2))
    tb = ax.table(cellText=cell, colLabels=["note", "row"] + cols, cellColours=colr, loc="center", cellLoc="center")
    tb.auto_set_font_size(False); tb.set_fontsize(8.6); tb.scale(1, 1.22)
    fig.text(0.055, 0.245, "Gate table at the band centre (4 s, steady window 0.5-2.0 s) against the DIAGNOSIS.md C1 rows and the Philharmonia reference", fontsize=10.5)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT, dpi=100)
print(OUT)
