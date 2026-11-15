"""Single note diagnosis chart: TARGET (Philharmonia) vs ALT-CELLO baseline vs ALT-CELLO best.

    nice /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/diagnose_chart.py NOTE BASETAG BESTTAG ITER
  NOTE G2|D3|A3; tags are condition tags of diagnose_run.py (their wavs are out/diag/<tag>__<NOTE>.wav). Writes
  /Users/alex/Music/foolish_cities_v5/alt_cello/charts/diagnose_<NOTE>_iter<ITER>.png and audio/diagnosis/<NOTE>_best.wav.
"""
import os, sys, shutil
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import diagnose as D, diag_render as R, diagnose_run as DR  # noqa: E402
SR = D.SR
C_REF, C_BASE, C_BEST = "#222222", "#d95f02", "#1b9e77"


def norm(nt):
    f, p = nt.spec(D.STEADY)
    g = np.sqrt(1.0 / D.M.band_power(f, p, 50, 800))
    return g


def main():
    note, tb, tg, it = sys.argv[1:5]
    ref = DR.ref_note(note)
    base = D.Note(os.path.join(R.OUTD, "%s__%s.wav" % (tb, note)), D.NOTE[note][1], "base")
    best = D.Note(os.path.join(R.OUTD, "%s__%s.wav" % (tg, note)), D.NOTE[note][1], "best")
    pre = lambda t: os.path.join(R.OUTD, "%s__%s" % (t, note))
    sm = [DR.summarize(ref, ref), DR.summarize(base, ref, pre(tb)), DR.summarize(best, ref, pre(tg))]
    fig = plt.figure(figsize=(16, 11))
    fig.suptitle("ALT-CELLO - %s single note diagnosis - TARGET Philharmonia vs ALT-CELLO - iteration %s" % (note, it), fontsize=21, fontweight="bold")
    gs = fig.add_gridspec(3, 6, left=0.05, right=0.985, top=0.92, bottom=0.05, hspace=0.38, wspace=0.55)
    ims = []
    for i, (nt, ttl) in enumerate([(ref, "TARGET Philharmonia"), (base, "ALT-CELLO baseline (%s)" % tb), (best, "ALT-CELLO best (%s)" % tg)]):
        ax = fig.add_subplot(gs[0, 2 * i:2 * i + 2])
        x = nt.x * norm(nt)
        S, F, T, im = ax.specgram(x[:int(2.5 * SR)], NFFT=4096, Fs=SR, noverlap=3584, cmap="magma", vmin=-110, vmax=-20, scale="dB")
        ax.set_ylim(0, 6000); ax.set_title(ttl, fontsize=12); ax.set_xlabel("s after onset"); ax.set_ylabel("Hz")
        ims.append(im)
    fig.colorbar(ims[-1], ax=fig.axes[:3], fraction=0.015, pad=0.01, label="dB (same scale, notes equalised on 50-800 Hz)")
    ax = fig.add_subplot(gs[1, 0:3])
    for nt, c, l in [(ref, C_REF, "TARGET Philharmonia"), (base, C_BASE, "ALT-CELLO baseline"), (best, C_BEST, "ALT-CELLO best")]:
        f, p = nt.spec(D.STEADY)
        g = D.M.band_power(f, p, 50, 800)
        ax.plot(f, 10 * np.log10(p / g + 1e-30), color=c, lw=0.9, label=l)
    ax.set_xlim(0, 6000); ax.set_ylim(-100, 10); ax.set_xlabel("Hz"); ax.set_ylabel("dB re 50-800 Hz power"); ax.legend(loc="upper right"); ax.set_title("long-term spectra, steady 0.5-2.0 s", fontsize=12)
    ax.grid(alpha=0.3)
    ax = fig.add_subplot(gs[1, 3:6])
    for nt, c, l in [(ref, C_REF, "TARGET"), (base, C_BASE, "baseline"), (best, C_BEST, "best")]:
        A = nt.hd["A"]; n = int(2.3 * SR) // int(0.1 * SR)
        for k, ls in enumerate(["-", "--", ":"]):
            tt = [(j + 0.5) * 0.1 for j in range(n)]
            y = [10 * np.log10(np.mean(A[k, j * 4800:(j + 1) * 4800] ** 2) / 2 / (np.mean(nt.x[j * 4800:(j + 1) * 4800] ** 2) + 1e-30) + 1e-30) for j in range(n)]
            ax.plot(tt, y, color=c, ls=ls, lw=1.3, label=("%s h%d" % (l, k + 1)) if True else None)
    ax.set_xlabel("s after onset"); ax.set_ylabel("dB re total"); ax.set_ylim(-35, 0); ax.grid(alpha=0.3)
    ax.set_title("h1 (solid) h2 (dashed) h3 (dotted) re total, 100 ms frames", fontsize=12); ax.legend(ncol=3, fontsize=7, loc="lower right")
    ax = fig.add_subplot(gs[2, :]); ax.axis("off")
    rows = [("jitter %", "jit", 2), ("shimmer dB", "shim", 2), ("HNR dB", "hnr", 1), ("centroid / f0", "cent_f0", 2), ("noise 0-1k re total dB", "nf0", 1),
            ("noise 1-2.5k", "nf1", 1), ("noise 2.5-5k", "nf2", 1), ("noise 5-10k", "nf3", 1), ("2-8 kHz first 0.4 s re steady dB", "tr_db", 1),
            ("time to -3 dB s", "t3db", 2), ("time to 10 one-slip periods s", "t_slip", 2), ("unmatched narrow bands >800 Hz", "nunm", 0),
            ("strongest unmatched excess dB", "unm_exc", 1), ("1-3 kHz re total dB", "b1_3", 1), ("3-8 kHz re total dB", "b3_8", 1),
            ("4-7 Hz modulation share", "mod4_7", 2), ("level sd dB", "lev_sd", 2)]
    cells = [[r[0]] + [D.fmt(float(s[r[1]]), r[2]) for s in sm] for r in rows]
    # pass column (best vs target)
    ok = DR.passes(sm[2], sm[0] | {"t_slip": 0.0}) if False else DR.passes(sm[2], dict(sm[0], t_slip=0.0))
    tb_ = ax.table(cellText=cells, colLabels=["metric", "TARGET", "baseline", "best"], loc="center", cellLoc="center", colWidths=[0.34, 0.14, 0.14, 0.14])
    tb_.auto_set_font_size(False); tb_.set_fontsize(9.5); tb_.scale(1, 1.18)
    ax.text(0.80, 0.5, "pass rules (best vs TARGET)\n" + "\n".join("%-8s %s" % (k, "PASS" if v else "FAIL") for k, v in ok.items()), transform=ax.transAxes, fontsize=12, family="monospace", va="center")
    out = "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/diagnose_%s_iter%s.png" % (note, it)
    fig.savefig(out, dpi=100)
    shutil.copy(os.path.join(R.OUTD, "%s__%s.wav" % (tg, note)), "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/diagnosis/%s_best.wav" % note)
    print("wrote", out, ok)


main()
