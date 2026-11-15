"""Writes DIAGNOSIS.md: the baseline tables (ALT-CELLO stage2c settings, 4 s re-render) beside Philharmonia, per pitch."""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import diagnose as D, diag_render as R, diagnose_run as DR  # noqa: E402
OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/diagnosis/DIAGNOSIS.md"
F = D.fmt


def refstats():
    L = open(os.path.join(R.OUTD, "refgates.tsv")).read().splitlines()
    k = L[0].split()[1:]
    A = np.array([[float(x) for x in l.split()[1:]] for l in L[1:]])
    return {n: np.percentile(A[:, i], [10, 50, 90]) for i, n in enumerate(k)}


def main():
    s = "# ALT-CELLO single note diagnosis - baseline\n\n"
    s += "Baseline = stage2c settings (open string, beta 0.1, v_B 0.1 m/s, F_B G2 0.67082 / D3 0.67082 / A3 0.49717 N, 30 ms ramp, noise 0.03, no vibrato) re-rendered for 4 s.\n"
    s += "Windows are seconds after the note onset (2 percent RMS rule, both notes): steady 0.5-2.0 s, early 0-0.4 s, ALT late 2.5-3.9 s. Definitions are in charts/diagnose.py.\n\n"
    rs = refstats()
    for p in DR.PITCHES:
        wav, pre = R.render(p, "base_")
        alt, rf = D.Note(wav, D.NOTE[p][1], "alt"), DR.ref_note(p)
        cols = [("ALT steady", alt, D.STEADY), ("REF steady", rf, D.STEADY), ("ALT early", alt, D.EARLY), ("REF early", rf, D.EARLY), ("ALT late", alt, (2.5, 3.9))]
        Ms = [D.window_metrics(n, w) for _, n, w in cols]
        s += "## %s (f0 ALT %.2f Hz, REF %.2f Hz)\n\n" % (p, Ms[0]["f0"], Ms[1]["f0"])
        s += "| metric | " + " | ".join(c[0] for c in cols) + " |\n|" + "---|" * 6 + "\n"
        def row(name, fn, pr=1): return "| %s | " % name + " | ".join(F(fn(m), pr) for m in Ms) + " |\n"
        s += row("jitter %", lambda m: m["jit_pct"], 3) + row("PPQ5 %", lambda m: m["ppq5"], 3) + row("shimmer dB", lambda m: m["shim_db"], 3)
        s += row("cycles", lambda m: float(m["ncyc"]), 0)
        s += row("median h1-12 level sd dB", lambda m: m["wander"], 2)
        for i, (lo, hi) in enumerate(D.BANDS_NF):
            s += row("noise %d-%d Hz re total dB" % (lo, hi), lambda m, i=i: m["nf_tot"][i])
        for i, (lo, hi) in enumerate(D.BANDS_NF):
            s += row("noise %d-%d Hz re harmonics dB" % (lo, hi), lambda m, i=i: m["nf_har"][i])
        s += row("HNR autocorr dB", lambda m: m["hnr_ac"]) + row("HNR residual dB", lambda m: m["hnr_res"])
        s += row("centroid Hz", lambda m: m["centroid"], 0) + row("centroid / f0", lambda m: m["cent_f0"], 2)
        for i in range(3):
            s += row("h%d re total dB (mean)" % (i + 1), lambda m, i=i: m["hbal_mean"][i]) + row("h%d re total dB (sd over 100 ms frames)" % (i + 1), lambda m, i=i: m["hbal_sd"][i], 2)
        s += "\n"
        at = [D.attack_metrics(n) for n in (alt, rf)]
        s += "Attack: ALT time to -3 dB %.2f s, REF %.2f s; 2-8 kHz power of the first 0.4 s re steady: ALT %+.1f dB, REF %+.1f dB.\n\n" % (at[0]["t3db"], at[1]["t3db"], at[0]["tr_db"], at[1]["tr_db"])
        ss = D.slip_stats(pre, Ms[0]["f0"])
        s += "Slip pattern (simulator): time to 10 consecutive one-slip periods strict %s s, lenient %s s; late one-slip fraction strict %s, lenient %s.\n\n" % (F(ss["strict"][0], 2), F(ss["lenient"][0], 2), F(ss["strict"][1], 2), F(ss["lenient"][1], 2))
        s += "Per harmonic (steady): level sd over 50 ms frames (dB) and modulation peak (Hz)\n\n| h | ALT sd | ALT mod Hz | REF sd | REF mod Hz |\n|---|---|---|---|---|\n"
        for k in range(12):
            s += "| %d | %.2f | %.1f | %.2f | %.1f |\n" % (k + 1, Ms[0]["h_sd"][k], Ms[0]["h_mod"][k], Ms[1]["h_sd"][k], Ms[1]["h_mod"][k])
        um, nun, npk = D.narrow_bands(alt, rf)
        s += "\nNarrow bands above 800 Hz of ALT not matched by REF within 6 dB (%d unmatched of %d peaks; the 5 strongest, level in dB re the 50-800 Hz power of the note, h = f/f0 and its deviation percent from an integer, m = f/(f0/beta) nearest integer and deviation percent):\n\n" % (nun, npk)
        s += "| f Hz | ALT dB | REF dB | excess dB | h | h dev % | m | m dev % |\n|---|---|---|---|---|---|---|---|\n"
        for r in um: s += "| %.0f | %.1f | %.1f | %.1f | %.2f | %.1f | %d | %.1f |\n" % (r["f"], r["lev"], r["ref"], r["exc"], r["h"], r["hdev"], r["mb"], r["mbdev"])
        # decay
        kw = dict(knobs=DR.stop_env(p), secs=4.1)
        dw, dp = R.render(p, "decay_", **kw)
        ring = [r for r in D.ring_analysis(dw) if r["t60"] > 1.5]
        s += "\nLong-ringing lines after the bow lifts at 2.5 s (T60 above 1.5 s, strongest first; level in dB of the 0.5 s frame at 2.55 s):\n\n| f Hz | level dB | T60 s | owner |\n|---|---|---|---|\n"
        for r in ring[:12]: s += "| %.1f | %.1f | %s | %s |\n" % (r["f"], r["lev0"], F(r["t60"], 1), r["owner"])
        nb = sum(1 for r in ring if r["owner"] == "body")
        s += "\n%d long-ringing lines, %d of them not on a string partial (body).\n\n" % (len(ring), nb)
    s += "## Gate set (lead's list; Philharmonia p10 / median / p90 over its 30 notes, my definitions)\n\n| gate | REF p10 | REF median | REF p90 | G2 | D3 | A3 |\n|---|---|---|---|---|---|---|\n"
    res = DR.load()
    for k in ["t_per", "sust_ac", "hnr", "b1_3", "b3_8", "att_tilt", "mod4_7", "mod10", "lev_sd", "ppq5", "shim", "peaky"]:
        kk = {"shim": "shim", "ppq5": "ppq5"}.get(k, k)
        vals = [res.get(("base", p), {}).get({"shim": "shim", "ppq5": "ppq5"}.get(k, k), float("nan")) for p in DR.PITCHES]
        s += "| %s | %s | %s |\n" % (k, " | ".join(F(float(x), 3) for x in rs[k]), " | ".join(F(v, 3) for v in vals))
    open(OUT, "w").write(s)
    print("wrote", OUT)


main()
