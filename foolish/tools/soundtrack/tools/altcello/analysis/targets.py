#!/usr/bin/env python3
"""targets.py: measure the benchmark cellos and write TARGETS.md, TARGETS.tsv, targets_overview.png.

Run: nice -n 15 /Users/alex/Dev/soundtrack-tools/venv/bin/python -I targets.py [source_name ...]   (no names: all sources)
Output dir /Users/alex/Music/foolish_cities_v5/alt_cello/targets/ (benchmarks are only read).
TARGET = median over the trusted sources (cello_notes, low_loud, mid_soft, raw set) of each source's median, with min and max across them.
TARGETS.tsv columns: metric, value, unit, source, spread_min, spread_max (source TARGET carries the spread).
"""
import os, re, sys, glob
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure as M

R3 = "/Users/alex/Music/foolish_cities_v5/lyria_round3/"
OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/targets/"
RAW = "/Users/alex/Dev/soundtrack-samples/lyria/cello/"
FLAT = "/Users/alex/Dev/soundtrack-samples/lyria/cello_flat_v6/"
# name, kind, path, trust ("trusted", "less trusted", "flattened, reference only")
SOURCES = [
    ("cello_notes", "clip", R3 + "cello_notes_6_warsaw.mp3", "trusted"),
    ("low_loud", "clip", R3 + "cello_low_loud_6_warsaw.mp3", "trusted"),
    ("mid_soft", "clip", R3 + "cello_mid_soft_6_warsaw.mp3", "trusted"),
    ("raw_set", "dir", RAW, "trusted"),
    ("mid_loud", "clip", R3 + "cello_mid_loud_6_warsaw.mp3", "less trusted"),
    ("low_soft", "clip", R3 + "cello_low_soft_6_warsaw.mp3", "less trusted"),
    ("high_medium", "clip", R3 + "cello_high_medium_6_warsaw.mp3", "less trusted"),
    ("flat_v6", "dir", FLAT, "flattened, reference only"),
]


def measure_source(name, kind, path):
    if kind == "clip":
        y = M.load_audio(path); notes, fc = M.find_runs(y, M.SR)
        return M.analyze_signal(y, M.SR, notes, name, f0_curve=fc)
    # directory of one-note files; midi from the name; cut from running passages, so attack and release are not measured
    rows, extras = [], []
    for p in sorted(glob.glob(path + "*.wav")):
        midi = int(re.search(r"_(\d+)(?:_rr\d+)?\.wav$", p).group(1)); y = M.load_audio(p)
        env = M.rms_env(y); floor = float(np.percentile(env, 10))
        r, ex = M.analyze_note(y, M.SR, 0.0, len(y) / M.SR, midi, floor, None, edge_silent=False)
        r["note"] = len(rows); r["file"] = os.path.basename(p); rows.append(r); extras.append(ex)
    return M.build_result(name, rows, extras, M.SR)


def main():
    os.makedirs(OUT, exist_ok=True); want = sys.argv[1:]
    results = {}
    for name, kind, path, trust in SOURCES:
        if want and name not in want: continue
        print("measuring", name, flush=True)
        res = measure_source(name, kind, path); results[name] = res
        M.write_outputs(res, OUT + "src_" + name)
    if want: return
    write_targets(results)


def write_targets(results):
    trusted = [n for n, k, p, t in SOURCES if t == "trusted"]
    tab = {n: {m: (v, u) for m, v, u in M.summary_metrics(results[n])} for n in results}
    metrics = []
    for n in results:
        for m in tab[n]:
            if m not in metrics: metrics.append(m)
    tgt = {}
    for m in metrics:
        vals = [tab[n][m][0] for n in trusted if m in tab[n] and not np.isnan(tab[n][m][0])]
        unit = next(tab[n][m][1] for n in results if m in tab[n])
        if vals: tgt[m] = (float(np.median(vals)), unit, min(vals), max(vals), len(vals))
    with open(OUT + "TARGETS.tsv", "w") as f:
        f.write("metric\tvalue\tunit\tsource\tspread_min\tspread_max\n")
        for m, (v, u, lo, hi, n) in tgt.items(): f.write("%s\t%.6g\t%s\tTARGET\t%.6g\t%.6g\n" % (m, v, u, lo, hi))
        for n in results:
            for m, (v, u) in tab[n].items():
                f.write("%s\t%.6g\t%s\t%s\t\t\n" % (m, v, u, n + ("" if dict((a, t) for a, k, p, t in SOURCES)[n] == "trusted" else " (%s)" % dict((a, t) for a, k, p, t in SOURCES)[n])))
    trust_of = dict((a, t) for a, k, p, t in SOURCES)
    cols = list(results)
    def head(first):
        return "| %s | TARGET | min..max | " % first + " | ".join(c + ("" if trust_of[c] == "trusted" else " *(%s)*" % trust_of[c]) for c in cols) + " |\n|---|---|---|" + "---|" * len(cols) + "\n"
    def row(m, label, fmt="%.2f"):
        t = tgt.get(m)
        s = "| %s | %s | %s | " % (label, fmt % t[0] if t else "-", ("%s..%s" % (fmt % t[2], fmt % t[3])) if t else "-")
        return s + " | ".join((fmt % tab[n][m][0]) if m in tab[n] and not np.isnan(tab[n][m][0]) else "-" for n in cols) + " |\n"
    L = ["# ALT-CELLO TARGET measurements from the benchmark cellos\n\n",
         "TARGET = median of the trusted sources (cello_notes, low_loud, mid_soft, raw_set), with the min..max across those four.\n",
         "Sources marked less trusted (mid_loud, low_soft, high_medium) and the flattened set (flat_v6: micro-variation numbers are NOT targets) are listed for reference only.\n",
         "Clips are cut into steady pitch runs of 0.8 s or longer (pyin); raw_set and flat_v6 are one note per file, cut from running passages (attack and release are not measurable there).\n",
         "Metric definitions: see the header of measure.py.\n\n",
         "Notes per source: " + ", ".join("%s %d" % (n, results[n]["S"]["n_notes"]) for n in cols) + "\n\n## Headline metrics\n\n", head("metric")]
    heads = [("attack_ms", "attack ms (1 to 90 percent)"), ("attack_10_90_ms", "attack ms (10 to 90 percent)"), ("rel_t20_s", "release T20 s"), ("rel_t40_s", "release T40 s"),
             ("rolloff85_hz", "rolloff85 Hz"), ("centroid_over_f0", "centroid / f0"), ("hnr_db", "HNR dB (autocorrelation)"), ("hnr_res_db", "HNR dB (residual)"),
             ("bow_noise_db", "bow noise 2-8 kHz re total dB"), ("pitch_micro_c", "pitch micro-variation c"), ("level_micro_db", "level micro-variation dB"),
             ("lvl_sd_slow_db", "slow level sd dB (ref def)"), ("pit_sd_slow_c", "slow pitch sd c (ref def)"),
             ("vib_present_frac", "fraction of notes with vibrato"), ("vib_rate_hz", "vibrato rate Hz"), ("vib_depth_c", "vibrato depth c peak"),
             ("vib_onset_s", "vibrato onset s"), ("vib_regularity", "vibrato regularity sd/mean"), ("wander_db", "harmonic wandering dB"),
             ("valley_db", "between-harmonics energy dB"), ("h_sd_notes_db", "sd across notes of h level dB"), ("attack_sd_ms", "sd across notes of attack ms"),
             ("f0_hz", "median f0 Hz"), ("cents_err", "pitch error vs nominal c"), ("n_notes", "notes")]
    L += [row(m, lb, "%.3f" if "regularity" in m or "frac" in m else "%.2f") for m, lb in heads]
    L += ["\n## Harmonic levels re h1 (dB, median)\n\n", head("harmonic")] + [row("h%d" % k, "h%d" % k, "%.1f") for k in range(2, 17)]
    L += ["\n## Partial frequency deviation from k x partial 1 (cents, median)\n\n", head("partial")] + [row("dev%d_c" % k, "partial %d" % k, "%.1f") for k in range(2, 11)]
    L += ["\n## Noise residual, 1/3-octave bands (dB re total power)\n\n", head("band Hz")] + [row("noise_db@%d" % round(f), "%d" % round(f), "%.1f") for f in M.THIRD_FC]
    L += ["\n## Five strongest peaks of the long-term spectral envelope (Hz, ranked by level)\n\n", head("rank")]
    L += [row("formant%d_hz" % i, "peak %d Hz" % i, "%.0f") for i in range(1, 6)]
    L += [row("bodypeak%d_hz" % i, "peak >= 300 Hz, rank %d" % i, "%.0f") for i in range(1, 6)]
    L += ["\n## Long-term average spectrum, 1/6-octave smoothed (dB re its peak)\n\n", head("Hz")] + [row("ltas_db@%d" % round(f), "%d" % round(f), "%.1f") for f in M.LTAS_F[::4]]
    open(OUT + "TARGETS.md", "w").write("".join(L))
    chart(results, tgt, tab, trust_of)


def chart(results, tgt, tab, trust_of):
    import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
    cols = list(results); cmap = plt.get_cmap("tab10")
    ls = {"trusted": "-", "less trusted": "--", "flattened, reference only": ":"}
    fig = plt.figure(figsize=(18, 12)); fig.suptitle("ALT-CELLO - TARGET measurements from the benchmark cellos (Lyria clips and raw sample set)", fontsize=20, fontweight="bold")
    gs = fig.add_gridspec(2, 2, hspace=0.3, wspace=0.16, top=0.92, bottom=0.06, left=0.06, right=0.98)
    def lab(n): return n + ("" if trust_of[n] == "trusted" else " (%s)" % trust_of[n])
    ax = fig.add_subplot(gs[0, 0])
    for i, n in enumerate(cols):
        if results[n]["ltas"] is not None: ax.plot(M.LTAS_F, results[n]["ltas"], ls[trust_of[n]], color=cmap(i), lw=1.6, label=lab(n))
    ax.plot(M.LTAS_F, [tgt["ltas_db@%d" % round(f)][0] for f in M.LTAS_F], "k-", lw=3.5, label="TARGET (median of trusted)")
    ax.set_xscale("log"); ax.set_ylim(-70, 3); ax.set_xlabel("Hz"); ax.set_ylabel("dB re peak"); ax.set_title("long-term spectral envelope (1/6 octave)", fontsize=14)
    ax.legend(fontsize=8); ax.grid(alpha=0.3)
    ax = fig.add_subplot(gs[0, 1]); ks = list(range(2, 17))
    for i, n in enumerate(cols): ax.plot(ks, [tab[n].get("h%d" % k, (np.nan,))[0] for k in ks], ls[trust_of[n]] + "o", color=cmap(i), lw=1.4, ms=4, label=lab(n))
    ax.plot(ks, [tgt["h%d" % k][0] for k in ks], "k-o", lw=3.5, label="TARGET"); ax.set_xlabel("harmonic k"); ax.set_ylabel("dB re h1")
    ax.set_title("median harmonic levels", fontsize=14); ax.grid(alpha=0.3); ax.legend(fontsize=8)
    ax = fig.add_subplot(gs[1, 0])
    for i, n in enumerate(cols): ax.plot(M.THIRD_FC, [tab[n].get("noise_db@%d" % round(f), (np.nan,))[0] for f in M.THIRD_FC], ls[trust_of[n]], color=cmap(i), lw=1.6, label=lab(n))
    ax.plot(M.THIRD_FC, [tgt["noise_db@%d" % round(f)][0] for f in M.THIRD_FC], "k-", lw=3.5, label="TARGET")
    ax.set_xscale("log"); ax.set_xlabel("Hz"); ax.set_ylabel("dB re total power"); ax.set_title("noise residual after removing harmonics (1/3 octave)", fontsize=14); ax.grid(alpha=0.3); ax.legend(fontsize=8)
    ax = fig.add_subplot(gs[1, 1]); ax.axis("off")
    def g(m, fmt="%.1f"):
        t = tgt.get(m); return ("%s  (%s to %s)" % (fmt % t[0], fmt % t[2], fmt % t[3])) if t else "-"
    txt = ["TARGET numbers (median of trusted sources, min to max)", "",
           "attack (1 to 90 percent)    " + g("attack_ms", "%.0f") + " ms", "HNR (autocorrelation)      " + g("hnr_db") + " dB", "HNR (residual)             " + g("hnr_res_db") + " dB",
           "bow noise 2-8 kHz re total  " + g("bow_noise_db") + " dB", "vibrato rate               " + g("vib_rate_hz", "%.2f") + " Hz", "vibrato depth (peak)       " + g("vib_depth_c") + " c",
           "vibrato onset              " + g("vib_onset_s", "%.2f") + " s", "vibrato regularity         " + g("vib_regularity", "%.3f"),
           "pitch micro-variation      " + g("pitch_micro_c", "%.2f") + " c", "level micro-variation      " + g("level_micro_db", "%.2f") + " dB",
           "harmonic wandering         " + g("wander_db", "%.2f") + " dB", "between-harmonics energy   " + g("valley_db") + " dB", "sd across notes, h level   " + g("h_sd_notes_db", "%.2f") + " dB",
           "rolloff85                  " + g("rolloff85_hz", "%.0f") + " Hz", "centroid / f0              " + g("centroid_over_f0", "%.2f"),
           "spectral peaks (Hz)        " + ", ".join("%.0f" % tgt["formant%d_hz" % i][0] for i in range(1, 6) if "formant%d_hz" % i in tgt),
           "peaks from 300 Hz up (Hz)  " + ", ".join("%.0f" % tgt["bodypeak%d_hz" % i][0] for i in range(1, 6) if "bodypeak%d_hz" % i in tgt)]
    ax.text(0.0, 1.0, "\n".join(txt), family="monospace", fontsize=12, va="top"); ax.set_title("TARGET table", fontsize=14)
    fig.savefig(OUT + "targets_overview.png", dpi=100); plt.close(fig)


if __name__ == "__main__":
    main()
