#!/usr/bin/env python3
"""targets.py: measure the benchmark cellos and write TARGETS.md, TARGETS.tsv, targets_overview.png.

Run: nice -n 15 /Users/alex/Dev/soundtrack-tools/venv/bin/python -I targets.py [source_name ...]   (no names: all sources)
Output dir /Users/alex/Music/foolish_cities_v5/alt_cello/targets/ (benchmarks are only read).
TARGET = median over the Philharmonia notes (see below) (cello_notes, low_loud, mid_soft, raw set) of each source's median, with min and max across them.
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


PHIL = "/Users/alex/Dev/soundtrack-samples/philharmonia/"
SOURCES.insert(0, ("philharmonia", "phil", PHIL, "TARGET source (Philharmonia solo cello)"))
# blind_ab_cello_MEASURES.md column A (Philharmonia in the Warsaw passage render): median over 29 notes of a legato render with hall,
# measure_cello.py definitions (rolloff85 on the body mean spectrum n_fft 8192, centroid/f0, h2 h3 re h1 largest bin within 3 percent,
# autocorrelation HNR, attack on a render keeping every third note, release T20/T40 on that render INCLUDING the 2.3 s hall tail so
# NOT comparable to the dry release here, vibrato rate and sd-based peak depth, slow level and pitch sd = 3 Hz lowpass + linear trend).
PASSAGE_REF = {"rolloff85_hz": 979, "centroid_over_f0": 1.81, "h2": -11.0, "h3": -10.8, "hnr_db": 16.6, "attack_10_90_ms": 260,
               "rel_t20_s": 0.95, "rel_t40_s": 2.02, "vib_rate_hz": 5.37, "vib_depth_c": 7.3, "lvl_sd_slow_db": 0.46, "pit_sd_slow_c": 2.0}


def measure_phil(name, path):
    """Philharmonia: pitch, spectra, noise, vibrato, micro-variation from the 0.25 s-cut wav (no fade in the body);
    attack and release from wav_full (the full note with its natural fade).  note end for the release = last frame within 3 dB of the sustain median."""
    import soundfile as sf
    rows, extras = [], []
    for line in open(path + "ph_cello_manifest.txt"):
        midi = int(line.split()[0]); y = M.load_audio(path + "wav/ph_cello_%d.wav" % midi); yf = M.load_audio(path + "wav_full/ph_cello_%d.wav" % midi)
        env = M.rms_env(y); on = int(np.argmax(env >= 0.1 * env.max())) * 0.01
        t0 = max(0.0, on - 0.05); floor = float(np.percentile(M.rms_env(yf), 10))
        r, ex = M.analyze_note(y, M.SR, t0, len(y) / M.SR - t0, midi, floor, None, edge_silent=False)
        ef = M.rms_env(yf); db = 20 * np.log10(ef + 1e-12); a, b = int((on + 0.3) * 100), int((on + 0.8) * 100)
        sus = float(np.median(db[a:b])); end = np.where(db >= sus - 3.0)[0][-1] * 0.01 + 0.01
        rf, _ = M.analyze_note(yf, M.SR, t0, end - t0, midi, floor, None, edge_silent=False)
        for k in ("attack_ms", "attack_10_90_ms", "rel_t20_s", "rel_t40_s"): r[k] = rf.get(k, np.nan)
        r["note"] = len(rows); r["file"] = "ph_cello_%d" % midi; rows.append(r); extras.append(ex)
    return M.build_result(name, rows, extras, M.SR)


def measure_source(name, kind, path):
    if kind == "phil": return measure_phil(name, path)
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
        cache = OUT + "src_" + name + ".tsv"
        print("measuring", name, flush=True)
        res = measure_source(name, kind, path); results[name] = res
        M.write_outputs(res, OUT + "src_" + name)
    if want: return
    write_targets(results)


def pct_spread(res, m):
    v = [r[m] for r in res["rows"] if m in r and not isinstance(r[m], str) and not np.isnan(r[m])]
    return (float(np.percentile(v, 10)), float(np.percentile(v, 90))) if len(v) >= 5 else (None, None)


def write_targets(results):
    trust_of = dict((a, t) for a, k, p, t in SOURCES)
    lyria = [n for n, k, p, t in SOURCES if t == "trusted"]
    tab = {n: {m: (v, u) for m, v, u in M.summary_metrics(results[n])} for n in results}
    metrics = []
    for n in results:
        for m in tab[n]:
            if m not in metrics: metrics.append(m)
    tgt, lref = {}, {}
    for m in metrics:
        unit = next(tab[n][m][1] for n in results if m in tab[n])
        if m in tab["philharmonia"] and not np.isnan(tab["philharmonia"][m][0]):
            lo, hi = pct_spread(results["philharmonia"], m); tgt[m] = (tab["philharmonia"][m][0], unit, lo, hi)
        vals = [tab[n][m][0] for n in lyria if m in tab[n] and not np.isnan(tab[n][m][0])]
        if vals: lref[m] = (float(np.median(vals)), unit, min(vals), max(vals))
    with open(OUT + "TARGETS.tsv", "w") as f:
        f.write("metric\tvalue\tunit\tsource\tspread_min\tspread_max\n")
        fm = lambda x: "" if x is None else "%.6g" % x
        for m, (v, u, lo, hi) in tgt.items(): f.write("%s\t%.6g\t%s\tTARGET\t%s\t%s\n" % (m, v, u, fm(lo), fm(hi)))
        for m, (v, u, lo, hi) in lref.items(): f.write("%s\t%.6g\t%s\tLYRIA_REF\t%s\t%s\n" % (m, v, u, fm(lo), fm(hi)))
        for m, v in PASSAGE_REF.items(): f.write("%s\t%.6g\t%s\tPASSAGE_REF\t\t\n" % (m, v, M.unit_of(m)))
        for n in results:
            for m, (v, u) in tab[n].items():
                f.write("%s\t%.6g\t%s\t%s\t\t\n" % (m, v, u, n + ("" if trust_of[n] == "trusted" else " (%s)" % trust_of[n])))
    cols = list(results)
    def head(first):
        return "| %s | TARGET (Philharmonia) | p10..p90 | LYRIA_REF | min..max | PASSAGE_REF | " % first + " | ".join(c + ("" if trust_of[c] == "trusted" else " *(%s)*" % trust_of[c]) for c in cols) + " |\n|---|---|---|---|---|---|" + "---|" * len(cols) + "\n"
    def row(m, label, fmt="%.2f"):
        t = tgt.get(m); l = lref.get(m); pr = PASSAGE_REF.get(m)
        rng = lambda d: ("%s..%s" % (fmt % d[2], fmt % d[3])) if d and d[2] is not None else "-"
        s = "| %s | %s | %s | %s | %s | %s | " % (label, fmt % t[0] if t else "-", rng(t), fmt % l[0] if l else "-", rng(l), fmt % pr if pr is not None else "-")
        return s + " | ".join((fmt % tab[n][m][0]) if m in tab[n] and not np.isnan(tab[n][m][0]) else "-" for n in cols) + " |\n"
    L = ["# ALT-CELLO TARGET measurements\n\n",
         "TARGET = median over the 30 notes of the Philharmonia SOLO cello (forte arco-normal, natural vibrato), spread = 10th to 90th percentile across notes (blank for metrics that exist only per source, e.g. LTAS, formants, sd across notes).\n",
         "LYRIA_REF = the earlier target: median of the trusted Lyria sources (cello_notes, low_loud, mid_soft, raw_set), spread = min..max across those four.\n",
         "PASSAGE_REF = blind_ab_cello_MEASURES.md column A (Philharmonia inside the Warsaw passage render: legato, hall, median of 29 notes).\n",
         "Definitions behind PASSAGE_REF (measure_cello.py): rolloff85 and centroid/f0 on the body mean spectrum; h2, h3 largest bin within 3 percent of k f0 re h1; HNR = autocorrelation peak definition; attack = 10 to 90 percent on a render keeping every third note; T20/T40 measured on that render INCLUDING the 2.3 s hall tail (not comparable to dry values); vibrato rate and sd-based peak depth; slow wobble = 3 Hz lowpass, linear trend removed.\n",
         "Philharmonia measurement: pitch, spectra, noise, vibrato and micro-variation from wav/ (last 0.25 s cut, so no fade inside the body); attack and release from wav_full/ (the full note with its natural fade; the full files are 0.25 s longer than the cut ones and start from silence); note end for the release = last frame within 3 dB of the sustain median.\n",
         "Less trusted Lyria sources and flat_v6 (flattened: micro-variation NOT a target) are reference only. Clips are cut into steady pitch runs of 0.8 s or longer (pyin); raw_set and flat_v6 are one note per file, cut from running passages.\n",
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
    chart(results, tgt, lref, tab, trust_of)


def chart(results, tgt, lref, tab, trust_of):
    import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
    cols = [c for c in results if c != "philharmonia"]; cmap = plt.get_cmap("tab10")
    ls = {"trusted": "-", "less trusted": "--", "flattened, reference only": ":"}
    fig = plt.figure(figsize=(18, 12)); fig.suptitle("ALT-CELLO - TARGET measurements: Philharmonia solo cello (TARGET) vs Lyria clips and raw set (LYRIA_REF)", fontsize=19, fontweight="bold")
    gs = fig.add_gridspec(2, 2, hspace=0.3, wspace=0.16, top=0.92, bottom=0.06, left=0.06, right=0.98)
    def lab(n): return n + ("" if trust_of[n] == "trusted" else " (%s)" % trust_of[n])
    def thick(ax, xs, ft, fl, marker=""):
        ax.plot(xs, [fl(x) for x in xs], "k--" + marker, lw=3, label="LYRIA_REF (median of trusted Lyria)")
        ax.plot(xs, [ft(x) for x in xs], "-" + marker, color="crimson", lw=4.5, label="TARGET (Philharmonia solo cello)")
    ax = fig.add_subplot(gs[0, 0])
    for i, n in enumerate(cols):
        if results[n]["ltas"] is not None: ax.plot(M.LTAS_F, results[n]["ltas"], ls[trust_of[n]], color=cmap(i), lw=1.0, alpha=0.7, label=lab(n))
    thick(ax, M.LTAS_F, lambda f: tgt["ltas_db@%d" % round(f)][0], lambda f: lref["ltas_db@%d" % round(f)][0])
    ax.set_xscale("log"); ax.set_ylim(-70, 3); ax.set_xlabel("Hz"); ax.set_ylabel("dB re peak"); ax.set_title("long-term spectral envelope (1/6 octave)", fontsize=14)
    ax.legend(fontsize=7); ax.grid(alpha=0.3)
    ax = fig.add_subplot(gs[0, 1]); ks = list(range(2, 17))
    for i, n in enumerate(cols): ax.plot(ks, [tab[n].get("h%d" % k, (np.nan,))[0] for k in ks], ls[trust_of[n]], color=cmap(i), lw=1.0, alpha=0.7, label=lab(n))
    thick(ax, ks, lambda k: tgt["h%d" % k][0], lambda k: lref["h%d" % k][0], "o")
    ax.set_xlabel("harmonic k"); ax.set_ylabel("dB re h1"); ax.set_title("median harmonic levels", fontsize=14); ax.grid(alpha=0.3); ax.legend(fontsize=7)
    ax = fig.add_subplot(gs[1, 0])
    for i, n in enumerate(cols): ax.plot(M.THIRD_FC, [tab[n].get("noise_db@%d" % round(f), (np.nan,))[0] for f in M.THIRD_FC], ls[trust_of[n]], color=cmap(i), lw=1.0, alpha=0.7, label=lab(n))
    thick(ax, M.THIRD_FC, lambda f: tgt["noise_db@%d" % round(f)][0], lambda f: lref["noise_db@%d" % round(f)][0])
    ax.set_xscale("log"); ax.set_xlabel("Hz"); ax.set_ylabel("dB re total power"); ax.set_title("noise residual after removing harmonics (1/3 octave)", fontsize=14); ax.grid(alpha=0.3); ax.legend(fontsize=7)
    ax = fig.add_subplot(gs[1, 1]); ax.axis("off")
    def g(m, fmt="%.1f"):
        t = tgt.get(m); l = lref.get(m)
        a = ("%s" % (fmt % t[0])) if t else "-"
        if t and t[2] is not None: a += " (%s..%s)" % (fmt % t[2], fmt % t[3])
        return "%-22s Lyria %s" % (a, fmt % l[0] if l else "-")
    rows = [("attack 1-90%  ms", "attack_ms", "%.0f"), ("attack 10-90% ms", "attack_10_90_ms", "%.0f"), ("release T20  s", "rel_t20_s", "%.2f"), ("release T40  s", "rel_t40_s", "%.2f"),
            ("HNR autocorr dB", "hnr_db", "%.1f"), ("HNR residual dB", "hnr_res_db", "%.1f"), ("bow noise dB", "bow_noise_db", "%.1f"),
            ("vibrato rate Hz", "vib_rate_hz", "%.2f"), ("vibrato depth c", "vib_depth_c", "%.1f"), ("vibrato onset s", "vib_onset_s", "%.2f"),
            ("pitch micro c", "pitch_micro_c", "%.2f"), ("level micro dB", "level_micro_db", "%.2f"), ("wandering dB", "wander_db", "%.2f"),
            ("between-harm dB", "valley_db", "%.1f"), ("rolloff85 Hz", "rolloff85_hz", "%.0f"), ("centroid/f0", "centroid_over_f0", "%.2f")]
    txt = ["TARGET = Philharmonia median (p10..p90)  |  LYRIA_REF median", ""] + ["%-18s %s" % (lb, g(m, f)) for lb, m, f in rows]
    txt.append("peaks from 300 Hz up   " + ", ".join("%.0f" % tgt["bodypeak%d_hz" % i][0] for i in range(1, 6) if "bodypeak%d_hz" % i in tgt))
    ax.text(0.0, 1.0, "\n".join(txt), family="monospace", fontsize=11, va="top"); ax.set_title("TARGET table", fontsize=14)
    fig.savefig(OUT + "targets_overview.png", dpi=100); plt.close(fig)


if __name__ == "__main__":
    main()
