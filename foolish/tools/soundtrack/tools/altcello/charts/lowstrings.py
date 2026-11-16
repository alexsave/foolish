"""ALT-CELLO lowstrings iteration 1 chart: sympathetic ringing and the low strings.
    python -I charts/lowstrings.py      -> /Users/alex/Music/foolish_cities_v5/alt_cello/charts/lowstrings_iter1.png
Data: out/ring_study (ring_study.py, ring_run.sh, ring_run2.sh), out/low_study (low_study.py, low_run.sh). The spectrograms render the held line
C2-G2-C3 (scores/held_line_low.txt, dry, default seed) with the shipped defaults and with the best tested low-string setting (nothing was accepted)."""
import os, sys, subprocess
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.gridspec import GridSpec
import scipy.io.wavfile as sw
import scipy.signal as ss
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(HERE)
import ring_table as RT, low_table as LT, onset_lib as L  # noqa: E402
OUTPNG = "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/lowstrings_iter1.png"
BEST = "q120"
BEST_KNOBS = "q2k.CG=120"
C = dict(base="#6b7280", a="#2563eb", b="#dc2626", g="#16a34a", o="#d97706")


def held(tag, knobs):
    w = os.path.join(L.TMP, "held_%s.wav" % tag)
    if not os.path.exists(w):
        os.environ["ALTCELLO_KNOBS"] = knobs
        L.render("held_line_low.txt", w)
    sr, y = sw.read(w)
    return sr, y.astype(float)


def main():
    rn = ["base", "lead1.5", "lead2", "stat2", "s0_2.4", "fifths"]
    R = {n: RT.load(n) for n in rn}
    ln = ["base", "q200", "q120", "q80", "zeta0.08", "zeta0.12", "zt5", "zt8"]
    P = {n: LT.load(n) for n in ln}
    fig = plt.figure(figsize=(16, 11), dpi=100)
    fig.suptitle("ALT-CELLO - sympathetic ringing and the low strings - iteration 1", fontsize=19, fontweight="bold", y=0.985)
    gs = GridSpec(4, 6, figure=fig, left=0.055, right=0.985, top=0.925, bottom=0.05, hspace=0.62, wspace=0.55)
    # (a) ring ratio against the coupling factor
    ax = fig.add_subplot(gs[0, 0:2])
    x = np.arange(len(rn)); w = 0.2
    for i, (k, w_, lab, col, al) in enumerate((("D3onG_E3", "w1_ring", "D3 on G (open D), 0.1 s", C["a"], 1.0), ("D3onG_E3", "w3_ring", "D3 on G, 0.3 s", C["a"], 0.4),
                                               ("A3onD_B3", "w1_ring", "A3 on D (open A), 0.1 s", C["b"], 1.0), ("A3onD_B3", "w3_ring", "A3 on D, 0.3 s", C["b"], 0.4))):
        ax.bar(x + (i - 1.5) * w, [R[n]["ring"][k][w_] for n in rn], w * 0.95, color=col, alpha=al, label=lab)
    base_d = R["base"]["ring"]["D3onG_E3"]["w1_ring"]
    ax.axhline(base_d - 6, color="k", ls="--", lw=1)
    ax.text(-0.45, base_d - 6.3, "6 dB below the D3 default", ha="left", va="top", fontsize=7)
    ax.set_xticks(x); ax.set_xticklabels(["default", "table\nmass x1.5", "table\nmass x2", "stat. mass\n1.5 to 3", "sigma0\n2.4", "pure\nfifths"], fontsize=8)
    ax.set_ylabel("open string re new note (dB)"); ax.set_title("(a) ring ratio against each coupling factor", fontsize=10, loc="left")
    ax.legend(fontsize=7, loc="upper center", ncol=2, bbox_to_anchor=(0.5, -0.28)); ax.set_ylim(-26, 8)
    # (b) transition times
    ax = fig.add_subplot(gs[0, 2:4])
    med = [R[n]["scale"]["median"] for n in rn]; mx = [R[n]["scale"]["max"] for n in rn]
    ax.bar(x - 0.18, med, 0.34, color=C["g"], label="median"); ax.bar(x + 0.18, mx, 0.34, color=C["o"], label="max")
    ax.axhline(med[0] / 2, color="k", ls="--", lw=1); ax.text(len(rn) - 0.5, med[0] / 2 - 25, "half the default median", ha="right", fontsize=8)
    for i, m in enumerate(med): ax.text(i - 0.18, m + 6, "%.0f" % m, ha="center", fontsize=8)
    ax.set_xticks(x); ax.set_xticklabels(["default", "x1.5", "x2", "stat", "s0 2.4", "fifths"], fontsize=8)
    ax.set_ylabel("ms to new pitch (10 c, 5 periods)"); ax.set_title("(b) scale: 7 vib-off changes x 3 takes", fontsize=10, loc="left"); ax.legend(fontsize=8, loc="upper center", ncol=2, bbox_to_anchor=(0.5, -0.28)); ax.set_ylim(0, 620)
    # (c) metrics text
    ax = fig.add_subplot(gs[0, 4:6]); ax.axis("off")
    ax.set_title("(c) decision on the coupling (all rules, one factor)", fontsize=10, loc="left")
    lines = ["%-9s ring0.1 D/A  band D3 A3   LTAS 1-3k  med" % "factor"]
    for n in rn:
        r = R[n]
        lines.append("%-9s %+5.1f %+5.1f   x%-4.1f x%-4.1f  %+5.1f  %4.0f" % (n, r["ring"]["D3onG_E3"]["w1_ring"], r["ring"]["A3onD_B3"]["w1_ring"], r["band_D3"]["ratio"], r["band_A3"]["ratio"], r["ltas"]["bands"][2] - R["base"]["ltas"]["bands"][2], r["scale"]["median"]))
    lines += ["", "rules: band within one grid step (x1.19), LTAS within 2 dB,", "AND ring halved (6 dB) or median transition halved.", "None passes: lead x1.5 only 0.6 dB on D3; lead x2 moves", "1-3 kHz by +2.1 dB and the median to 105 ms.", "Defaults kept. During the note the open string is", "-4.4 dB (D) and +1.8 dB (A) re the bowed one."]
    ax.text(0, 0.98, "\n".join(lines), va="top", family="monospace", fontsize=7.6)
    # (d) low-string sweeps
    names = ["default", "Q 200", "Q 120", "Q 80", "zt .08", "zt .12", "Zt/Z0 5", "Zt/Z0 8"]
    xs = np.arange(len(ln))
    def panel(pos, title, getter, thr, thrlab, ylab, logy=False):
        ax = fig.add_subplot(gs[1, pos])
        for key, col, lab, off in (("C2 open", C["a"], "C2 open", -0.18), ("G2 on C", C["b"], "G2 on C", 0.18)):
            ax.bar(xs + off, [getter(P[n], key) for n in ln], 0.34, color=col, label=lab)
        if thr is not None: ax.axhline(thr, color="k", ls="--", lw=1.2); ax.text(len(ln) - 0.5, thr, thrlab, ha="right", va="bottom", fontsize=8)
        ax.set_xticks(xs); ax.set_xticklabels(names, fontsize=7, rotation=40, ha="right"); ax.set_ylabel(ylab); ax.set_title(title, fontsize=10, loc="left")
        return ax
    ax = panel(slice(0, 2), "(d1) Helmholtz band at beta 0.1, v_B 0.1 (32 forces)", lambda p, k: p["band_" + k.replace(" ", "_")]["ratio"], 4.0, "x4 needed", "contiguous band ratio"); ax.legend(fontsize=8)
    ax = panel(slice(2, 4), "(d2) Guettler cells at most 0.1 s (of 72)", lambda p, k: p["guettler_" + k.replace(" ", "_")]["fast"], 4, "4 needed", "cells"); ax.set_ylim(0, 6)
    ax.text(0.5, 0.3, "zero in every setting\n(steady cells at all: 21-42 of 72)", transform=ax.transAxes, ha="center", fontsize=9)
    ax = panel(slice(4, 6), "(d3) HNR of the expert note (median of 3 takes)", lambda p, k: p["note"][k]["hnr_med"], 20.0, "20 dB needed", "HNR dB")
    # (e) Guettler maps
    def gmap(pos, name, semis, label):
        ax = fig.add_subplot(gs[2, pos])
        rows = [l.split() for l in open(os.path.join(ROOT, "out", "low_study", "grid_%s_%d.txt" % (name, semis))) if not l.startswith("#")]
        F = sorted(set(float(r[0]) for r in rows)); A = sorted(set(float(r[1]) for r in rows))
        Z = np.full((len(F), len(A)), np.nan); S = np.zeros((len(F), len(A)))
        for r in rows:
            i, j = F.index(float(r[0])), A.index(float(r[1]))
            ta = float(r[2]); S[i, j] = int(r[6]); Z[i, j] = ta if (int(r[6]) and ta >= 0) else np.nan
        ax.imshow(np.where(S > 0, 0.5, 0.0), cmap="Greys", vmin=0, vmax=1.4, origin="lower", aspect="auto")
        im = ax.imshow(np.ma.masked_invalid(Z), cmap="viridis_r", vmin=0.0, vmax=1.0, origin="lower", aspect="auto")
        ax.set_xticks(range(len(A))); ax.set_xticklabels(["%g" % a for a in A], fontsize=7); ax.set_yticks(range(0, len(F), 2)); ax.set_yticklabels(["%.2f" % F[i] for i in range(0, len(F), 2)], fontsize=7)
        ax.set_xlabel("bow acceleration (m/s^2)", fontsize=8); ax.set_ylabel("force (N)", fontsize=8)
        ax.set_title(label, fontsize=9, loc="left"); return im
    gmap(slice(0, 1), "base", 7, "(e1) G2 on C, default"); gmap(slice(1, 2), BEST, 7, "(e2) G2 on C, Q2k 120")
    gmap(slice(2, 3), "base", 0, "(e3) C2 open, default"); im = gmap(slice(3, 4), BEST, 0, "(e4) C2 open, Q2k 120")
    cax = fig.add_axes([0.655, 0.30, 0.008, 0.12]); fig.colorbar(im, cax=cax).set_label("time to periodic (s)", fontsize=7); cax.tick_params(labelsize=7)
    ax = fig.add_subplot(gs[2, 4:6]); ax.axis("off")
    ax.set_title("(f) low-string numbers (C2 open | G2 on C)", fontsize=10, loc="left")
    t = ["%-9s band     fast/steady  HNR       t_per s" % "setting"]
    for r in LT.table(ln): t.append("%-9s %-4s|%-4s %-3s|%-3s    %-4s|%-4s %-4s|%-4s" % (r[0], r[1], r[6], r[2].split('/')[0], r[7].split('/')[0], r[3], r[8], r[4], r[9]))
    t += ["", "Rule: >=4 cells <=0.1 s on both, band >=x4, HNR >=20,", "1-3 kHz not >5 dB duller. Cells: 0 everywhere, HNR", "<20 on G2 everywhere. Nothing qualifies; best", "tested: Q2k 120 (HNR 19.4 | 18.4, t_per 0.60 | 0.21)."]
    ax.text(0, 0.98, "\n".join(t), va="top", family="monospace", fontsize=7.4)
    # (g) spectrograms
    for pos, (tag, kn, lab) in enumerate((("def", "", "(g1) held line C2-G2-C3, shipped defaults (candidate 2)"), ("q120", BEST_KNOBS, "(g2) the same, best tested setting (Q2k 120; not accepted)"))):
        sr, y = held(tag, kn)
        f, tt, Sx = ss.spectrogram(y, sr, nperseg=8192, noverlap=6144)
        ax = fig.add_subplot(gs[3, pos * 3:pos * 3 + 3])
        db = 10 * np.log10(Sx + 1e-20); db -= db.max()
        ax.pcolormesh(tt, f, np.clip(db, -80, 0), cmap="magma", shading="auto", vmin=-80, vmax=0)
        ax.set_ylim(0, 2500); ax.set_xlabel("s"); ax.set_ylabel("Hz"); ax.set_title(lab, fontsize=10, loc="left")
    fig.savefig(OUTPNG)
    print(OUTPNG)


main()
