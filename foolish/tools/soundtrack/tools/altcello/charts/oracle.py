"""ALT-CELLO oracle chart (iteration 1): Schelleng diagrams of our finite difference bowed string against an independent MSW waveguide.

Reads the text files written by charts/oracle_run.sh (sweep_tool) from DATA and writes oracle_iter1.png.
Run: /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/oracle.py
"""
import os, re, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch

DATA = "/Users/alex/Music/foolish_cities_v5/alt_cello/oracle"
OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/oracle_iter1.png"
BETAS = [0.05, 0.10, 0.20]
CFG = [("i", "i full"), ("ii", "ii no noise"), ("iii", "iii no torsion"), ("iv", "iv 1-pt bow"), ("v", "v stiff /10"), ("vi", "vi all of ii-v")]
# D3 constants (instrument_cello.c): Z0 from the oracle log, v_B 0.1, mu_s 1.2, mu_d 0.35 (the friction curve's sliding asymptote)
VB, MUS, MUD = 0.1, 1.2, 0.35
C_HELM, C_MULT, C_NONE, C_IRR = "#1b9e77", "#d95f02", "#7570b3", "#999999"


def load(path):
    rows = []
    for ln in open(path):
        if ln.startswith("#") or not ln.strip():
            continue
        p = ln.split()
        rows.append(dict(F=float(p[0]), ours=int(p[1]), simple=int(p[2]), cmin=int(p[3]), cmax=int(p[4]), slip=float(p[5]), corr=float(p[6])))
    return rows


def oracle_info(beta):
    for ln in open(os.path.join(DATA, "B1_D3_b%.2f.log" % beta)):
        m = re.match(r"oracle D3: Z0 ([\d.]+) g ([\d.]+) p ([\d.]+)", ln)
        if m:
            return float(m.group(1)), float(m.group(2)), float(m.group(3))
    raise RuntimeError("no oracle line")


def band(rows, key, contiguous=True):
    """F_min, F_max of the Helmholtz points: the longest contiguous run on the force grid (default: isolated stray points below or
    above the band are ignored), or the whole span first..last Helmholtz point."""
    ok = [bool(r[key]) for r in rows]
    F = [r["F"] for r in rows]
    if not any(ok):
        return None
    if not contiguous:
        idx = [i for i, o in enumerate(ok) if o]
        return F[idx[0]], F[idx[-1]]
    best, cur, start, bs = 0, 0, 0, 0
    for i, o in enumerate(ok):
        if o:
            if cur == 0:
                start = i
            cur += 1
            if cur > best:
                best, bs = cur, start
        else:
            cur = 0
    return F[bs], F[bs + best - 1]


def cls_color(r):
    if r["cmax"] == 0:
        return C_NONE
    if r["cmin"] >= 2:
        return C_MULT
    return C_IRR


def theory(Z0, r_rt):
    """Schelleng: F_max(beta), F_min(beta). R from the round trip reflection magnitude r: R = Z0 (1 + r) / (1 - r)."""
    R = Z0 * (1 + r_rt) / (1 - r_rt)
    b = np.geomspace(0.035, 0.3, 50)
    fmax = 2 * Z0 * VB / ((MUS - MUD) * b)
    fmin = Z0 ** 2 * VB / (2 * R * (MUS - MUD) * b ** 2)
    return b, fmin, fmax, R


def draw_map(ax, files, title, th):
    for bi, beta in enumerate(BETAS):
        rows = load(files[bi])
        for key, off, mk in (("ours", 1.09, "o"), ("simple", 0.92, "s")):
            for r in rows:
                if r[key]:
                    ax.plot(r["F"], beta * off, mk, color=C_HELM, ms=6.5, mec="none")
                else:
                    ax.plot(r["F"], beta * off, mk, color=cls_color(r), ms=4.5, alpha=0.8, mec="none")
    b, fmin, fmax, R = th
    ax.plot(fmax, b, "k--", lw=1.2)
    ax.plot(fmin, b, "k:", lw=1.5)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(0.015, 6.5)
    ax.set_ylim(0.037, 0.28)
    ax.set_yticks(BETAS)
    ax.set_yticklabels(["0.05", "0.10", "0.20"])
    ax.minorticks_off()
    ax.set_xlabel("bow force F_B (N)")
    ax.set_ylabel("beta (circle: our classifier, square: simple)")
    ax.set_title(title, fontsize=11)
    ax.grid(alpha=0.25)


def main():
    Z0, g, p = oracle_info(0.10)
    f0 = 146.83
    w0 = 2 * np.pi * f0 / 96000.0
    h = g * (1 - p) / np.sqrt(1 - 2 * p * np.cos(w0) + p * p)
    r_rt = h * h  # nut and bridge reflection per round trip
    th = theory(Z0, r_rt)

    fig = plt.figure(figsize=(16, 11), dpi=100)
    fig.suptitle("ALT-CELLO - playability: our FD bowed string vs an MSW waveguide oracle - Schelleng diagrams - D3 - iteration 1", fontsize=17, fontweight="bold", y=0.985)
    gs = fig.add_gridspec(3, 6, left=0.055, right=0.985, top=0.925, bottom=0.07, hspace=0.42, wspace=1.2, height_ratios=[1.05, 1.0, 0.55])

    orc = [os.path.join(DATA, "B1_D3_b%.2f.txt" % b) for b in BETAS]
    full = [os.path.join(DATA, "B2_D3_b%.2f_i.txt" % b) for b in BETAS]
    strip = [os.path.join(DATA, "B2_D3_b%.2f_vi.txt" % b) for b in BETAS]
    ax = fig.add_subplot(gs[0, 0:2]); draw_map(ax, orc, "MSW oracle (rigid ends, point bow, no torsion, no stiffness)", th)
    ax2 = fig.add_subplot(gs[0, 2:4]); draw_map(ax2, full, "ours (i): stiffness + torsion + 1 cm bow + noise", th)
    ax3 = fig.add_subplot(gs[0, 4:6]); draw_map(ax3, strip, "ours (vi): no noise, no torsion, 1-pt bow, stiffness /10", th)
    leg = [Line2D([], [], marker="o", ls="", color=C_HELM, ms=7, label="Helmholtz (large marker)"),
           Line2D([], [], marker="o", ls="", color=C_MULT, ms=5, label="multiple slips per period"),
           Line2D([], [], marker="o", ls="", color=C_NONE, ms=5, label="no slip (choked)"),
           Line2D([], [], marker="o", ls="", color=C_IRR, ms=5, label="irregular / raucous"),
           Line2D([], [], color="k", ls="--", label="Schelleng F_max"),
           Line2D([], [], color="k", ls=":", label="Schelleng F_min (R from loss: %.0f kg/s)" % th[3])]
    ax.legend(handles=leg, loc="lower left", fontsize=7.5, framealpha=0.9)

    # bar chart: F_max/F_min per configuration, per beta, both classifiers
    axb = fig.add_subplot(gs[1, 0:3])
    labels = ["oracle"] + [c[1] for c in CFG]
    paths = {"oracle": lambda b: os.path.join(DATA, "B1_D3_b%.2f.txt" % b)}
    cols = {0.05: "#a6cee3", 0.10: "#1f78b4", 0.20: "#08306b"}
    width = 0.13
    for li, lab in enumerate(labels):
        for bi, beta in enumerate(BETAS):
            f = paths["oracle"](beta) if li == 0 else os.path.join(DATA, "B2_D3_b%.2f_%s.txt" % (beta, CFG[li - 1][0]))
            rows = load(f)
            for ki, key in enumerate(("ours", "simple")):
                bd = band(rows, key)
                ratio = bd[1] / bd[0] if bd else 0.0
                x = li + (bi * 2 + ki - 2.5) * width
                axb.bar(x, ratio, width * 0.95, color=cols[beta], hatch="" if key == "ours" else "///", edgecolor="white" if key == "ours" else "k", lw=0.4)
    axb.axhline(1, color="k", lw=0.6)
    axb.set_xticks(range(len(labels)))
    axb.set_xticklabels(labels, fontsize=9)
    axb.set_ylabel("F_max / F_min of the contiguous band")
    axb.set_title("playable force ratio per configuration (longest contiguous Helmholtz run, grid step x1.34)", fontsize=10.5)
    axb.legend(handles=[Patch(color=cols[b], label="beta %.2f" % b) for b in BETAS] + [Patch(facecolor="w", edgecolor="k", label="our classifier"), Patch(facecolor="w", edgecolor="k", hatch="///", label="simple classifier")],
               fontsize=8, ncol=5, loc="upper right")
    axb.grid(axis="y", alpha=0.25)
    axb.set_ylim(0, axb.get_ylim()[1] * 1.12)

    # waveforms
    axw = fig.add_subplot(gs[1, 3:6])
    for name, col, lab in (("oracle", "#d95f02", "MSW oracle"), ("ours", "#1f78b4", "ours (i), string only")):
        f = os.path.join(DATA, "wave_%s.txt" % name)
        if os.path.exists(f):
            d = np.loadtxt(f)
            axw.plot(d[:, 0], d[:, 1], color=col, lw=1.2, label=lab)
    axw.set_xlabel("time (ms), last 3 periods of a 4 s run")
    axw.set_ylabel("string velocity at the bow (m/s)")
    axw.set_title("same force (0.67 N), beta 0.1, v_B 0.1 m/s", fontsize=10.5)
    axw.legend(fontsize=9, loc="center", bbox_to_anchor=(0.38, 0.45))
    axw.grid(alpha=0.25)

    # table
    axt = fig.add_subplot(gs[2, :])
    axt.axis("off")
    lines = []
    for li, lab in enumerate(labels):
        cells = [lab.ljust(16)]
        for beta in BETAS:
            f = paths["oracle"](beta) if li == 0 else os.path.join(DATA, "B2_D3_b%.2f_%s.txt" % (beta, CFG[li - 1][0]))
            rows = load(f)
            s = []
            for key in ("ours", "simple"):
                bd = band(rows, key)
                s.append("%.2f-%.2f (x%.1f)" % (bd[0], bd[1], bd[1] / bd[0]) if bd else "none")
            cells.append(("%s | %s" % (s[0], s[1])).ljust(44))
        lines.append("  ".join(cells))
    txt = ("Helmholtz band F_min-F_max (N) (ratio), longest contiguous run; D3, v_B 0.1 m/s, 20 forces 0.02-5 N, 6 s runs; columns beta 0.05 | 0.10 | 0.20, each 'our classifier | simple classifier'\n"
           "Theory (Schelleng, mu_s 1.2, mu_d 0.35, R = %.0f kg/s from the loss): beta 0.05: %.3f-%.2f, 0.10: %.3f-%.2f, 0.20: %.4f-%.2f N\n" % ((th[3],) + tuple(x for b in BETAS for x in (float(np.interp(b, th[0], th[1])), float(np.interp(b, th[0], th[2])))))
           + "\n".join(lines))
    axt.text(0.0, 1.0, txt, family="monospace", fontsize=8.2, va="top")
    fig.text(0.055, 0.045, "Reading: (vi) reproduces the oracle's band (identical at beta 0.1, 0.27-1.17 N; within one grid step at 0.05 and 0.2), so the finite difference solve itself is not the defect.\n"
             "In the full model torsion raises F_min (0.27 -> 0.87 N, step iii) and bow noise lowers F_max (2.8 -> 1.2 N, step ii); the simple classifier changes nothing by more than one grid step.\n"
             "The oracle itself spans only x4.3 at beta 0.1, against x97 from Schelleng's formula with the same loss: friction and loss limit the band.", fontsize=9.5, va="top")
    fig.savefig(OUT, dpi=100)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
