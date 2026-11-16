"""Richness iteration 1 chart: note-axis spectrograms, LTAS, floor, string T60, gap closed per factor.
    python -I charts/richness.py   (needs out/rich/res_c4.pkl, res_comb.pkl, tab_*.txt, out/rich/t60_old.txt, t60_new.txt, and the rendered notes)"""
import os, sys, pickle, re
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, "/Users/alex/Music/foolish_cities_v5/shared_tools/spectro")
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import rich_lib as R
from notespec import show_note_spectrogram
OUTP = "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/richness_iter1.png"
c4 = pickle.load(open(R.OUT + "/res_c4.pkl", "rb"))
c5 = pickle.load(open(R.OUT + "/res_comb.pkl", "rb"))
fig = plt.figure(figsize=(16, 13))
fig.suptitle("ALT-CELLO - spectral richness: bow noise spectrum, string loss shape, body high end - iteration 1", fontsize=17, fontweight="bold")
gs = fig.add_gridspec(4, 6, height_ratios=[1, 1, 0.85, 0.85], hspace=0.5, wspace=1.1)
for r, (key, nm) in enumerate((("D2", "d2"), ("G2c", "g2c"))):
    for c, (lab, y) in enumerate((("TARGET Philharmonia", R.ref_note(key)), ("candidate 4", R.M.load_audio(R.OUT + "/c4_%s_s0.wav" % nm)), ("candidate 5", R.M.load_audio(R.OUT + "/comb_%s_s0.wav" % nm)))):
        ax = fig.add_subplot(gs[r, c * 2:c * 2 + 2])
        on = R.onset(y)
        show_note_spectrogram(ax, y[int(max(on - 0.1, 0) * R.SR):int((on + 1.6) * R.SR)], R.SR, title="%s: %s" % (lab, "D2 (C string +2)" if r == 0 else "G2 stopped on C"), vrange=80, octaves=7)
        ax.set_xlabel("s", fontsize=8)
for ax, key in ((fig.add_subplot(gs[2, 0:2]), "D2"), (fig.add_subplot(gs[2, 2:4]), "G2c")):
    for res, lab, col in ((c5, "candidate 5", "tab:green"), (c4, "candidate 4", "tab:orange")):
        ax.plot(R.BANDS, np.mean([m["ltas"] for m in res[key]["ours"]], axis=0), color=col, label=lab)
    ax.plot(R.BANDS, c4[key]["ref"]["ltas"], "k", label="Philharmonia")
    for (lo, hi), col in zip(R.RANGES, ("#cde", "#fed", "#dfd", "#fdd")):
        ax.axvspan(lo, hi, color=col, alpha=0.5)
    ax.set_xscale("log"); ax.set_ylim(-85, 3); ax.set_title("LTAS (third octave, re own peak) " + key, fontsize=9); ax.set_xlabel("Hz"); ax.set_ylabel("dB"); ax.legend(fontsize=7, loc="upper right")
    for (lo, hi), lb in zip(R.RANGES, R.RLAB):
        d4 = np.mean([R.ltas_ranges(m["ltas"], c4[key]["ref"]["ltas"]) for m in c4[key]["ours"]], axis=0)
        d5 = np.mean([R.ltas_ranges(m["ltas"], c5[key]["ref"]["ltas"]) for m in c5[key]["ours"]], axis=0)
    ax.text(0.02, 0.04, "gap c4 -> c5 (dB):\n" + "\n".join("%s: %+.1f -> %+.1f" % (lb, a, b) for lb, a, b in zip(R.RLAB, d4, d5)), transform=ax.transAxes, fontsize=6.5)
ax = fig.add_subplot(gs[2, 4:6])
x = np.arange(len(R.OCT))
for res, lab, col in ((c4, "candidate 4", "tab:orange"), (c5, "candidate 5", "tab:green")):
    ax.plot(x, np.nanmean([np.nanmean([m["floor"] for m in res[k]["ours"]], axis=0) for k in res], axis=0), "o-", color=col, label=lab)
ax.plot(x, np.nanmean([res_ for res_ in [np.array(c4[k]["ref"]["floor"]) for k in c4]], axis=0), "ks-", label="Philharmonia")
ax.set_xticks(x); ax.set_xticklabels(["0.5-1", "1-2", "2-4", "4-8", "8-16 kHz"], fontsize=8); ax.set_ylabel("dB re harmonic peaks"); ax.set_title("between-harmonic floor per octave (mean of 4 notes)", fontsize=9); ax.legend(fontsize=7)
ax = fig.add_subplot(gs[3, 0:2])
for fn, lab, col in (("t60_old.txt", "old loss (sigma1 k^2)", "tab:orange"), ("t60_new.txt", "relaxation loss (flat Q)", "tab:blue")):
    d = np.loadtxt(os.path.join(R.OUT, fn), usecols=(2, 3, 5), dtype=float, converters={0: lambda s: 0}) if False else None
    rows = [l.split() for l in open(os.path.join(R.OUT, fn)) if l.startswith("G2")]
    f = np.array([float(r[2]) for r in rows]); t = np.array([float(r[5]) for r in rows])
    ax.loglog(f, t, ".-", color=col, label=lab, ms=3)
ff = np.geomspace(100, 12000, 50)
ax.loglog(ff, np.log(1000) / (np.pi * ff / 300.0), "k--", label="target: Q 300 flat")
ax.set_xlabel("Hz"); ax.set_ylabel("T60 of the partial, s"); ax.set_title("G2 string: partial T60 from a pluck, rigid bridge", fontsize=9); ax.legend(fontsize=7, loc="lower left")
ax = fig.add_subplot(gs[3, 2:6])
fac = [("F1 bow noise 16 kHz, 0.02", "f1_16000_0.02"), ("F2 relaxation string loss", "f2"), ("F3 stat. modes to 16 kHz", "f3_statf1"), ("F3 eta exponent 0.15", "f3_etaexp"), ("F3 mass exponent 0.3", "f3_massexp"),
       ("F4 bridge R x0.7", "f4_br07"), ("F4 bridge R x1.5", "f4_br15"), ("F4 table masses x0.5", "f4_lead05"), ("F4 stat. masses x0.5", "f4_stat05"), ("F5 damping touch (null, see text)", None),
       ("bow width 5 mm", "x_bw5"), ("COMBINED (candidate 5)", "comb")]
def gap(tag):
    return float(re.search(r"mean \|gap\| ([\d.]+)", open(os.path.join(R.OUT, "tab_%s.txt" % tag)).read()).group(1))
g0 = gap("c4")
vals = [(g0 - gap(t)) if t else 0.0 for _, t in fac]
ax.barh(range(len(fac)), vals, color=["tab:green" if v > 0.3 else "tab:gray" for v in vals])
ax.set_yticks(range(len(fac))); ax.set_yticklabels([n for n, _ in fac], fontsize=7)
ax.yaxis.set_tick_params(pad=2); ax.invert_yaxis()
ax.set_xlabel("mean |LTAS gap| closed over the 4 ranges and 4 notes, dB (candidate 4: %.2f)" % g0, fontsize=8); ax.set_title("gap closed per factor, each alone against candidate 4", fontsize=9)
ax.axvline(0, color="k", lw=0.5)
fig.text(0.01, 0.005, "candidate 5 vs 4 (mean of D2, G2c, D3, A3): 5-10 kHz gap %+.1f -> %+.1f dB, 1.6-5 kHz %+.1f -> %+.1f dB, 0.5-1.6 kHz %+.1f -> %+.1f dB; HNR D2 %.1f/%.1f (ref %.1f), G2c %.1f/%.1f (ref %.1f), D3 %.1f/%.1f (ref %.1f), A3 %.1f/%.1f (ref %.1f)" % (
    *[v for i in (3, 2, 1) for v in (np.mean([R.ltas_ranges(m["ltas"], c4[k]["ref"]["ltas"])[i] for k in c4 for m in c4[k]["ours"]]), np.mean([R.ltas_ranges(m["ltas"], c5[k]["ref"]["ltas"])[i] for k in c5 for m in c5[k]["ours"]]))],
    *[v for k in ("D2", "G2c", "D3", "A3") for v in (np.mean([m["hnr"] for m in c4[k]["ours"]]), np.mean([m["hnr"] for m in c5[k]["ours"]]), c4[k]["ref"]["hnr"])]), fontsize=7)
plt.savefig(OUTP, dpi=100)
print(OUTP)
