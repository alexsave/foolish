"""FE body one-factor test (iteration 1): the solved body_fem modes below 2.5 kHz (380) against the statistical body, FE-A (monopole straight from r_net) and FE-B (the FE modes
through our radiation filters), the richness measurement of rich_lib.py (D2, G2 on C, D3, A3; expert, forte, vibrato on, dry, 3 seeds) against Philharmonia.

    /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/fe_body.py [tables|chart|spectro|all]

Needs out/rich/res_fe_stat.pkl, res_fe_a.pkl, res_fe_b.pkl (charts/fe_run.sh), out/fe_admit_*.txt (fe_tool admit), the play_*.txt sweeps of charts/fe_play.sh.
Writes /Users/alex/Music/foolish_cities_v5/alt_cello/charts/fe_body_iter1.png (+ _spectro.png) and out/rich/fe_tables.txt."""
import os, sys, pickle
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, "/Users/alex/Music/foolish_cities_v5/shared_tools/spectro")
import rich_lib as R  # noqa: E402
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

OUTD = "/Users/alex/Music/foolish_cities_v5/alt_cello/charts"
PLAY = "/Users/alex/Music/foolish_cities_v5/alt_cello/fe_body"
TAGS = [("fe_stat", "STAT (default)", "#555555"), ("fe_a", "FE-A monopole", "#d95f02"), ("fe_b", "FE-B filtered", "#1b9e77")]
NOTE_KEYS = ["D2", "G2c", "D3", "A3"]
LABEL = {"D2": "D2 (C2+2 on C)", "G2c": "G2 on C", "D3": "D3 on G", "A3": "A3 on D"}
BANDS = np.array(R.BANDS)
RTF = {"fe_stat": 1.1, "fe_a": 0.9, "fe_b": 0.9}  # audio s per cpu s of the whole render of D3 (altcello render, measured)


def load():
    return {t: pickle.load(open(os.path.join(R.OUT, "res_%s.pkl" % t), "rb")) for t, _, _ in TAGS}


def mean_ltas(res, which):
    if which == "ref":
        return np.mean([res[k]["ref"]["ltas"] for k in NOTE_KEYS], axis=0)
    return np.mean([m["ltas"] for k in NOTE_KEYS for m in res[k]["ours"]], axis=0)


def gaps(res):
    """mean over notes of the per-note range gaps (ours - ref) and per-third-octave gaps"""
    rows = R.summarize(res)
    rng = np.mean([rows[k]["rng"] for k in NOTE_KEYS], axis=0)
    band = np.mean([np.mean([m["ltas"] - res[k]["ref"]["ltas"] for m in res[k]["ours"]], axis=0) for k in NOTE_KEYS], axis=0)
    return rng, band, rows


def tables(D):
    out = []
    out.append("FE body one-factor test: richness measurement (rich_lib.py), 3 seeds, sustained 0.5-1.4 s, ours - Philharmonia, each curve minus its own peak third octave\n")
    for t, nm, _ in TAGS:
        rng, band, rows = gaps(D[t])
        out.append(R.fmt("%s" % nm, rows))
        out.append("")
    out.append("Per range, mean over the four notes (dB):   200-500   500-1600   1600-5000   5000-10000   mean|gap|")
    allg = {}
    for t, nm, _ in TAGS:
        rng, band, rows = gaps(D[t])
        allg[t] = rng
        out.append("  %-16s %+8.1f %+10.1f %+11.1f %+12.1f %10.2f" % (nm, rng[0], rng[1], rng[2], rng[3], float(np.mean(np.abs(rng)))))
    out.append("Gap closed re STAT (|gap STAT| - |gap body|, dB; negative = worse):")
    for t, nm, _ in TAGS[1:]:
        c = np.abs(allg["fe_stat"]) - np.abs(allg[t])
        out.append("  %-16s %+8.1f %+10.1f %+11.1f %+12.1f %10.2f" % (nm, c[0], c[1], c[2], c[3], float(np.mean(c))))
    out.append("")
    out.append("Third-octave gaps 100-2000 Hz (ours - ref, dB, mean over the 4 notes x 3 seeds); then per note for FE-B")
    sel = [i for i, fc in enumerate(BANDS) if 99 <= fc <= 2100]
    bands = {t: gaps(D[t])[1] for t, _, _ in TAGS}
    out.append("  fc Hz     " + "  ".join("%-16s" % nm for _, nm, _ in TAGS) + "   FE-B - STAT")
    for i in sel:
        out.append("  %7.0f   " % BANDS[i] + "  ".join("%+16.1f" % bands[t][i] for t, _, _ in TAGS) + "   %+8.1f" % (bands["fe_b"][i] - bands["fe_stat"][i]))
    out.append("  mean |gap| 100-2000: " + "  ".join("%s %.2f" % (nm, np.mean(np.abs(bands[t][sel]))) for t, nm, _ in TAGS))
    out.append("  mean |gap| 200-1000: " + "  ".join("%s %.2f" % (nm, np.mean(np.abs(bands[t][[i for i in sel if 190 <= BANDS[i] <= 1010]]))) for t, nm, _ in TAGS))
    sel3 = [i for i in sel if 190 <= BANDS[i] <= 1010]
    def pernote(t, ii):
        return float(np.mean([np.mean(np.abs(np.mean([m["ltas"] - D[t][k]["ref"]["ltas"] for m in D[t][k]["ours"]], axis=0)[ii])) for k in NOTE_KEYS]))
    out.append("  per-note |gap| (no cancelling between notes) 100-2000: " + "  ".join("%s %.2f" % (nm, pernote(t, sel)) for t, nm, _ in TAGS))
    out.append("  per-note |gap| (no cancelling between notes) 200-1000: " + "  ".join("%s %.2f" % (nm, pernote(t, sel3)) for t, nm, _ in TAGS))
    out.append("")
    out.append("Per note gaps in 100-2000 Hz third octaves (ours - ref, mean over seeds)")
    for k in NOTE_KEYS:
        out.append("  " + LABEL[k])
        for t, nm, _ in TAGS:
            g = np.mean([m["ltas"] - D[t][k]["ref"]["ltas"] for m in D[t][k]["ours"]], axis=0)
            out.append("    %-16s " % nm + " ".join("%+5.0f" % g[i] for i in sel))
    out.append("    band fc          " + " ".join("%5.0f" % BANDS[i] for i in sel))
    out.append("")
    out.append("(b) between-harmonic floor re harmonic peaks (dB) per octave 0.5-1 / 1-2 / 2-4 / 4-8 / 8-16 kHz, mean over notes (reference first)")
    ref = np.nanmean([D["fe_stat"][k]["ref"]["floor"] for k in NOTE_KEYS], axis=0)
    out.append("  %-16s " % "Philharmonia" + " ".join("%6.1f" % v for v in ref))
    for t, nm, _ in TAGS:
        f = np.nanmean([np.nanmean([m["floor"] for m in D[t][k]["ours"]], axis=0) for k in NOTE_KEYS], axis=0)
        out.append("  %-16s " % nm + " ".join("%6.1f" % v for v in f) + "   (ours - ref: " + " ".join("%+5.1f" % v for v in (f - ref)) + ")")
    out.append("")
    out.append("(c)(d) HNR dB and time to periodic s (mean over seeds / median over seeds)")
    out.append("  %-6s %-9s %-9s %-9s %-9s   | %-9s %-9s %-9s %-9s" % ("note", "HNR ref", "STAT", "FE-A", "FE-B", "tper ref", "STAT", "FE-A", "FE-B"))
    for k in NOTE_KEYS:
        h = [np.mean([m["hnr"] for m in D[t][k]["ours"]]) for t, _, _ in TAGS]
        tp = [np.median([m["tper"] for m in D[t][k]["ours"]]) for t, _, _ in TAGS]
        out.append("  %-6s %-9.1f %-9.1f %-9.1f %-9.1f   | %-9.2f %-9.2f %-9.2f %-9.2f" % (k, D["fe_stat"][k]["ref"]["hnr"], h[0], h[1], h[2], D["fe_stat"][k]["ref"]["tper"], tp[0], tp[1], tp[2]))
    return "\n".join(out)


def band_of(path):
    rows = [ln.split() for ln in open(path) if not ln.startswith("#") and ln.strip()]
    F = [float(r[0]) for r in rows]
    ok = [int(r[1]) == 1 and int(r[2]) == 1 for r in rows]
    best = (0, -1)
    i = 0
    while i < len(F):
        if ok[i]:
            j = i
            while j + 1 < len(F) and ok[j + 1]: j += 1
            if j - i + 1 > best[0]: best = (j - i + 1, i)
            i = j + 1
        else: i += 1
    return sum(ok), best[0], (F[best[1]] if best[0] else 0.0), (F[best[1] + best[0] - 1] if best[0] else 0.0)


def play_table():
    out = ["(e) Helmholtz band, beta 0.1, v_B 0.1 m/s, 32 forces 0.03-5 N, both classifiers; count of Helmholtz forces / longest contiguous run / its Fmin-Fmax (N)"]
    for s in ("D3", "A3"):
        for tag, nm in (("stat", "STAT"), ("a", "FE-A"), ("b", "FE-B")):
            p = os.path.join(PLAY, "play_%s_%s.txt" % (tag, s))
            if os.path.exists(p):
                n, run, lo, hi = band_of(p)
                out.append("  %s %-5s %2d forces, contiguous %2d, %.3f - %.3f N%s" % (s, nm, n, run, lo, hi, "" if not run else "  (ratio %.1f)" % (hi / lo)))
    return "\n".join(out)


def read_admit(v):
    rows = np.loadtxt(os.path.join(R.ROOT, "out", "fe_admit_%d.txt" % v))
    return rows[:, 0], rows[:, 1:]


def chart(D):
    fig = plt.figure(figsize=(16, 11), dpi=100)
    fig.suptitle("ALT-CELLO - solved FE body (body_fem, 380 modes below 2.5 kHz) vs the statistical body - one-factor test - iteration 1", fontsize=15, fontweight="bold", y=0.985)
    gs = fig.add_gridspec(2, 2, left=0.06, right=0.985, top=0.925, bottom=0.07, hspace=0.30, wspace=0.17)
    # admittance
    sub = gs[0, 0].subgridspec(4, 1, hspace=0.12)
    ax = fig.add_subplot(sub[:3, 0])
    f0, y0 = read_admit(0)
    f1, y1 = read_admit(1)
    ax.semilogx(f0, y0[:, 0], color="#555555", lw=1.2, label="STAT (one notch for all strings: C = A)")
    ax.semilogx(f1, y1[:, 0], color="#c0392b", lw=1.0, label="FE, C notch")
    ax.semilogx(f1, y1[:, 3], color="#2471a3", lw=1.0, label="FE, A notch")
    ax.set_xlim(40, 3000)
    plt.setp(ax.get_xticklabels(), visible=False)
    ax.set_ylabel("|Y_uu| bridge notch, dB re 1 m/s/N")
    ax.set_title("Bridge admittance (u): C and A notches", fontsize=11)
    ax.grid(True, which="both", alpha=0.25)
    ax.legend(fontsize=8, loc="upper right")
    ax2 = fig.add_subplot(sub[3, 0], sharex=ax)
    ax2.semilogx(f1, y1[:, 3] - y1[:, 0], color="#2471a3", lw=0.9)
    ax2.axhline(0, color="k", lw=0.6)
    ax2.set_ylim(-12, 12)
    ax2.set_ylabel("A - C, dB", fontsize=9)
    ax2.set_xlabel("Hz")
    ax2.grid(True, which="both", alpha=0.25)
    # LTAS
    ax = fig.add_subplot(gs[0, 1])
    sel = BANDS >= 99
    ax.semilogx(BANDS[sel], mean_ltas(D["fe_stat"], "ref")[sel], color="k", lw=2.4, label="TARGET Philharmonia")
    for t, nm, c in TAGS:
        ax.semilogx(BANDS[sel], mean_ltas(D[t], "ours")[sel], color=c, lw=1.6, label=nm)
    ax.axvline(2500, color="#999999", ls="--", lw=1)
    ax.text(2350, -3, "FE | statistical\ncrossover 2.5 kHz", fontsize=8, color="#666666", ha="right", va="top")
    ax.set_xlim(100, 10500)
    ax.set_xlabel("third octave centre, Hz")
    ax.set_ylabel("dB re the peak third octave")
    ax.set_title("LTAS 100 Hz - 10 kHz, mean of D2, G2, D3, A3 (sustained part, 3 seeds)", fontsize=11)
    ax.grid(True, which="both", alpha=0.25)
    ax.legend(fontsize=9, loc="lower left")
    # gap bars
    ax = fig.add_subplot(gs[1, 0])
    labs = R.RLAB
    w = 0.26
    for j, (t, nm, c) in enumerate(TAGS):
        g = gaps(D[t])[0]
        b = ax.bar(np.arange(4) + (j - 1) * w, g, w, color=c, label=nm)
        for x, v in zip(np.arange(4) + (j - 1) * w, g):
            ax.text(x, v + (0.8 if v >= 0 else -0.8), "%+.1f" % v, ha="center", va="bottom" if v >= 0 else "top", fontsize=8)
    ax.axhline(0, color="k", lw=1)
    ax.set_xticks(range(4))
    ax.set_xticklabels([l + " Hz" for l in labs])
    ax.set_ylabel("LTAS gap, ours - real, dB (0 = closed)")
    ax.set_title("Gap per band (mean of 4 notes x 3 seeds)", fontsize=11)
    ax.set_ylim(-36, 14)
    ax.grid(True, axis="y", alpha=0.25)
    ax.legend(fontsize=9, loc="lower left")
    # metrics text
    ax = fig.add_subplot(gs[1, 1])
    ax.axis("off")
    lines = []
    hdr = "%-30s %8s %8s %8s" % ("metric (mean of 4 notes)", "STAT", "FE-A", "FE-B")
    lines.append(hdr)
    g = {t: gaps(D[t])[0] for t, _, _ in TAGS}
    lines.append("%-30s %+8.1f %+8.1f %+8.1f" % ("gap 200-500 Hz", g["fe_stat"][0], g["fe_a"][0], g["fe_b"][0]))
    lines.append("%-30s %+8.1f %+8.1f %+8.1f" % ("gap 500-1600 Hz", g["fe_stat"][1], g["fe_a"][1], g["fe_b"][1]))
    lines.append("%-30s %+8.1f %+8.1f %+8.1f" % ("gap 1.6-5 kHz", g["fe_stat"][2], g["fe_a"][2], g["fe_b"][2]))
    lines.append("%-30s %+8.1f %+8.1f %+8.1f" % ("gap 5-10 kHz", g["fe_stat"][3], g["fe_a"][3], g["fe_b"][3]))
    lines.append("%-30s %8.2f %8.2f %8.2f" % ("mean |gap| (4 bands)", *[float(np.mean(np.abs(g[t]))) for t, _, _ in TAGS]))
    sel2 = [i for i, fc in enumerate(BANDS) if 190 <= fc <= 1010]
    lines.append("%-30s %8.2f %8.2f %8.2f" % ("|gap| 200-1000, per note", *[float(np.mean([np.mean(np.abs(np.mean([m["ltas"] - D[t][k]["ref"]["ltas"] for m in D[t][k]["ours"]], axis=0)[sel2])) for k in NOTE_KEYS])) for t, _, _ in TAGS]))
    for k in ("hnr", "tper"):
        lab = "HNR dB (real %.1f)" % np.mean([D["fe_stat"][n]["ref"]["hnr"] for n in NOTE_KEYS]) if k == "hnr" else "t periodic s (real %.2f)" % np.mean([D["fe_stat"][n]["ref"]["tper"] for n in NOTE_KEYS])
        vals = [np.mean([np.median([m[k] for m in D[t][n]["ours"]]) for n in NOTE_KEYS]) for t, _, _ in TAGS]
        lines.append("%-30s %8.2f %8.2f %8.2f" % (lab, *vals))
    fl = [np.nanmean([np.nanmean([m["floor"] for m in D[t][n]["ours"]], axis=0) for n in NOTE_KEYS], axis=0) for t, _, _ in TAGS]
    lines.append("%-30s %8.1f %8.1f %8.1f" % ("floor 1-2 kHz re peaks dB", fl[0][1], fl[1][1], fl[2][1]))
    lines.append("%-30s %8.1f %8.1f %8.1f" % ("floor 2-4 kHz re peaks dB", fl[0][2], fl[1][2], fl[2][2]))
    lines.append("%-30s %8.1f %8.1f %8.1f" % ("real-time factor (render)", RTF["fe_stat"], RTF["fe_a"], RTF["fe_b"]))
    txt = "\n".join(lines) + "\n\n" + play_table().replace("(e) ", "Playability: ").replace("; count of Helmholtz forces / longest contiguous run / its Fmin-Fmax (N)", "")
    txt += "\n\nFE-A: p = gain rho V'' / (4 pi d), V = sum r_net q; FE-B: compact modes (air>0.5, f<250 Hz)\nthrough the ka filter, the rest through the plate filter with sign(r_net).\nBoth: crossover 2.5 kHz, level matched to STAT at 2.0-2.5 kHz by one scale each."
    ax.text(0.0, 1.0, txt, family="monospace", fontsize=8.2, va="top", ha="left", transform=ax.transAxes)
    fig.savefig(os.path.join(OUTD, "fe_body_iter1.png"), dpi=100)
    plt.close(fig)


def spectro(D):
    import librosa
    from notespec import cqt_db
    fmin = librosa.note_to_hz("C1")
    keys = [("ref", "Philharmonia")] + [(t, nm) for t, nm, _ in TAGS]
    fig, axs = plt.subplots(4, 4, figsize=(20, 16), dpi=90, sharey=True)
    fig.suptitle("ALT-CELLO FE body iteration 1 - note-axis spectrograms C1-C9, 65 dB range below each file's own peak (seed 0 of the rich set)", fontsize=15, fontweight="bold", y=0.995)
    for r, k in enumerate(NOTE_KEYS):
        nm = R.NOTES[k][2]
        for c, (t, lab) in enumerate(keys):
            if t == "ref":
                y = R.ref_note(k)
            else:
                y = R.M.load_audio(os.path.join(R.OUT, "%s_%s_s0.wav" % (t, nm)))
            on = R.onset(y)
            a, b = int(max(on - 0.15, 0) * R.SR), int((on + 1.6) * R.SR)
            y = y[a:b]
            db, tt, f = cqt_db(y, R.SR, hop=512, fmin=fmin, octaves=8)
            ax = axs[r, c]
            ax.imshow(db, origin="lower", aspect="auto", extent=[0, tt[-1], 0, db.shape[0]], vmin=db.max() - 65, vmax=db.max(), cmap="magma")
            ticks = [(s * 36 / 12.0, "C%d" % (1 + s // 12)) for s in range(0, 8 * 12 + 1, 12)]
            ax.set_yticks([p for p, _ in ticks])
            ax.set_yticklabels([l for _, l in ticks], fontsize=9)
            for p, _ in ticks:
                ax.axhline(p, color="white", lw=0.5, alpha=0.45)
            if r == 0:
                ax.set_title(lab, fontsize=13)
            if c == 0:
                ax.set_ylabel(LABEL[k] + "\nnote", fontsize=11)
            if r == 3:
                ax.set_xlabel("s from 0.15 s before the onset")
    fig.tight_layout(rect=(0, 0, 1, 0.985))
    fig.savefig(os.path.join(OUTD, "fe_body_iter1_spectro.png"), dpi=90)
    plt.close(fig)


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    D = load()
    if what in ("tables", "all"):
        t = tables(D) + "\n\n" + play_table()
        open(os.path.join(R.OUT, "fe_tables.txt"), "w").write(t + "\n")
        print(t)
    if what in ("chart", "all"):
        chart(D)
    if what in ("spectro", "all"):
        spectro(D)
