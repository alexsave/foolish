#!/usr/bin/env python3
"""chorus.py - the se_12 finding: every voice between 0:50 and 1:45 and the vocals-stem line between 64 and 78 s.

What it does: for 3_seoul/se_12_song_traditional_fusion it writes OUT/se_12_chorus.md with the voices present from 50 to
105 s (stem, rule, pitch range, first and last note, the guess), the stem levels per 5 s, and for the line that the
separator put in the vocals stem between 64 and 78 s: pitch range, envelope, harmonic spectrum, vibrato, glides between
notes, formant-like peaks and whether they move with pitch, noise share, the note table and note sequence, and the
measured evidence for and against four readings (a sung voice, b bowed fiddle such as a haegeum, c wind instrument,
d synth lead).  The readings use the thresholds of the guess.py rule table; the page names nothing beyond them.
Called by inventory.py --chorus (it passes the inventory module so Clip and the grid helpers are shared);
CLI: not run on its own.
Failure modes: the vocals stem holds whatever the separator called voice-like, so the line may be a leak of another
  instrument; with fewer than 5 alone notes the measurement says "not measurable"; reference ranges quoted in the
  evidence (the human singing range) are general knowledge, marked as such.  Deterministic; no randomness.
"""
import os

import numpy as np

import describe as ds
import guess as gs
import voices as vc


def _rms(x):
    return 20 * np.log10(np.sqrt(np.mean(np.asarray(x, np.float64) ** 2)) + 1e-12)


def evidence(R, G):
    """Per reading: (for, against) lists of measured statements. Thresholds are those of guess.py."""
    g = lambda k, s="median": (R.get(k) or {}).get(s) if isinstance(R.get(k), dict) else R.get(k)
    att, vs, vr = g("attack_s"), R.get("vib_share"), g("vib_rate_hz")
    gp = [x for x in (R.get("glide_share"), R.get("portamento_share")) if x is not None]
    gp = max(gp) if gp else None
    nh, noise, slope, inh = g("n_harm_30db"), g("noise_share"), g("slope_db_oct"), g("inharm_cents")
    oe, blc, fm = g("odd_even_db"), g("bright_level_corr"), R.get("formant")
    steady = []
    for r in R["per_note"]:
        if r["dur"] >= 0.4:
            m = (r["track_t"] >= 0.25 * r["dur"]) & (r["track_t"] <= 0.75 * r["dur"]) & np.isfinite(r["track_c"])
            if m.sum() >= 5:
                steady.append(float(np.std(r["track_c"][m])))
    st = float(np.median(steady)) if steady else None
    f = lambda v, d=1: "n/a" if v is None else "%.*f" % (d, v)
    E = {}

    def mk():
        A, B, U = [], [], []

        def add(cond, text):
            (U if cond is None else A if cond else B).append(text)
        return A, B, U, add

    long_n = R["vib_notes_measured"]
    vtxt = "vibrato share %s at %s Hz over %d notes of 0.6 s or more (the table asks for 0.3 or more at 4.5 to 7.5 Hz)" % (f(vs, 2), f(vr), long_n)
    vib_ok = None if vs is None else (vs >= 0.3 and (vr is None or 4.5 <= vr <= 7.5))
    gtxt = "glide share %s, portamento share %s (the table asks for 0.2 or more)" % (f(R.get("glide_share"), 2), f(R.get("portamento_share"), 2))
    A, B, U, add = mk()
    add(vib_ok, vtxt)
    if fm and fm.get("moves_with_f0") is not None:
        add(not fm["moves_with_f0"], "formant-like peak %.0f Hz, correlation with f0 %.2f over %.1f semitones: %s" % (
            fm["peak_hz_median"], fm["corr_with_f0"], fm["f0_spread_semitones"], "moves with f0" if fm["moves_with_f0"] else "stays put as f0 changes"))
    else:
        add(None, "formant-like peak: %s" % ("inconclusive (peak cv %.2f, correlation with f0 %.2f, f0 median %.0f Hz; above about 500 Hz the harmonic spacing is wider than a formant)" % (fm["cv"], fm["corr_with_f0"], fm["f0_median"]) if fm else "not measurable"))
    A.append("the separator put the line in the vocals stem (a label of the separator, not proof)")
    add(None if noise is None else noise > 0.04, "noise share %s (the table asks for over 0.04)" % f(noise, 3))
    add(None if gp is None else gp >= 0.2, gtxt)
    p10, p90 = R["pitch"]["p10"], R["pitch"]["p90"]
    add(48 <= p10 and p90 <= 84, "pitch range of the middle 80 percent of notes MIDI %d to %d against a human singing range of about MIDI 48 to 84 (general knowledge, not measured)" % (p10, p90))
    add(None if att is None else att >= 0.03, "attack %s ms" % f(None if att is None else att * 1000, 0))
    E["a) sung voice"] = (A, B, U)
    A, B, U, add = mk()
    add(None if att is None else 0.04 <= att <= 0.4, "attack %s ms (the table asks for 40 to 400)" % f(None if att is None else att * 1000, 0))
    add(vib_ok, vtxt)
    add(None if gp is None else gp >= 0.2, gtxt)
    add(None if nh is None else nh >= 8, "%s harmonics within 30 dB of harmonic 1 (the table asks for 8 or more)" % f(nh, 0))
    add(None if noise is None else noise > 0.04, "noise share %s (the table asks for over 0.04)" % f(noise, 3))
    E["b) bowed fiddle such as a haegeum"] = (A, B, U)
    A, B, U, add = mk()
    add(None if att is None else 0.03 <= att <= 0.12, "attack %s ms (the table asks for 30 to 120)" % f(None if att is None else att * 1000, 0))
    add(None if blc is None else blc > 0.4, "centroid against level correlation %s (the table asks for over 0.4)" % f(blc, 2))
    add(None if oe is None else oe > 6, "odd-to-even %s dB (over 6 for a clarinet-like tube)" % f(oe))
    add(None if (nh is None or noise is None) else (nh <= 4 and noise > 0.08), "%s harmonics within 30 dB and noise share %s (flute-like needs 4 or fewer and over 0.08)" % (f(nh, 0), f(noise, 3)))
    add(None if gp is None else gp < 0.2, "glide and portamento: " + gtxt.replace("(the table asks for 0.2 or more)", "(a high share counts against a wind reading)"))
    E["c) wind instrument"] = (A, B, U)
    A, B, U, add = mk()
    add(None if slope is None else -9 <= slope <= -3, "harmonic slope %s dB/octave (the table asks for -9 to -3)" % f(slope))
    add(None if inh is None else inh < 6, "inharmonicity %s cents (under 6 for an exact-harmonic oscillator)" % f(inh))
    add(None if st is None else st < 10, "pitch steadiness inside notes: standard deviation %s cents (median of %d notes)" % (f(st), len(steady)))
    add(None if vs is None else vs < 0.15, "vibrato share %s over %d long notes (the table asks for under 0.15 for a static oscillator)" % (f(vs, 2), long_n))
    add(None if noise is None else noise < 0.05, "noise share %s (under 0.05)" % f(noise, 3))
    E["d) synth lead"] = (A, B, U)
    return E, st, len(steady)


def write_chorus(a, inv):
    out = a.out
    city, name = "3_seoul", "se_12_song_traditional_fusion"
    mp3 = next(os.path.join(r, city, name + ".mp3") for r in a.clips if os.path.exists(os.path.join(r, city, name + ".mp3")))
    clip = inv.Clip(city, name, a.analysis, mp3, a.prompts)
    L = []
    w = L.append
    ws, we = 50.0, 105.0
    w("# se_12 chorus: voices from 0:50 to 1:45 and the line around 1:10")
    w("")
    w("The owner called the voice around 1:10 phenomenal and later asked about the sweet instrument playing in the chorus.")
    w("The prompt for this song asked for these instrument words: %s." % ", ".join(gs.prompt_instruments(clip.prompt)))
    w("The prompt excluded: %s." % ", ".join(gs.prompt_instruments(clip.prompt, negated=True)))
    w("These are words in the prompt text, not things heard.")
    w("All numbers below are measurements of Demucs stems and basic-pitch notes.")
    w("")
    w("## Stem levels per 5 s (dBFS RMS of the stem, and against the mix)")
    w("")
    w("| from s | mix | bass | drums | other | vocals | vocals re mix dB |")
    w("|---|---|---|---|---|---|---|")
    sr = clip.sr
    t = ws
    while t < we - 1e-9:
        a0, b0 = int(t * sr), int(min(t + 5, we) * sr)
        m = _rms(clip.mix[a0:b0])
        row = [_rms(clip.audio[s][a0:b0].mean(axis=1)) for s in ("bass", "drums", "other", "vocals")]
        w("| %.0f | %.1f | %s | %.1f |" % (t, m, " | ".join("%.1f" % v for v in row), row[3] - m))
        t += 5
    w("")
    w("## Voices present from 50 to 105 s")
    w("")
    vs, left = vc.split_all(clip.stem_notes, clip.audio, sr, clip.mix, ws, we, clip.grid["bpm"] or 150.0)
    w("Voices follow the rules of voices.py; the vocals-stem rule: %s." % left.get("vocals", "voice present"))
    w("")
    w("| stem | voice | notes | first | last | pitch range | best guess (confidence) | rule |")
    w("|---|---|---|---|---|---|---|---|")
    for v in vs:
        stem_notes = clip.stem_notes.get(v["stem"], np.zeros((0, 4)))
        if v["kind"] == "pitched":
            R = ds.describe_voice(clip.audio[v["stem"]], sr, v["notes"], stem_notes)
            pr = "%s to %s (median %s)" % (inv.mname(R["pitch"]["lo"]), inv.mname(R["pitch"]["hi"]), inv.mname(R["pitch"]["median"]))
        else:
            R = ds.describe_drum(clip.audio["drums"], sr, list(v["notes"][:, 0]), v["name"], None)
            pr = "onsets"
        G = gs.guess(R, v["stem"], v["name"], clip.prompt)
        w("| %s | %s | %d | %.2f | %.2f | %s | %s (%s) | %s |" % (v["stem"], v["name"], len(v["notes"]), v["notes"][:, 0].min(), v["notes"][:, 0].max(), pr, G["text"], G["confidence"], v["rule"]))
    w("")
    # the vocals-stem line 64-78
    vn = vc.window_notes(clip.stem_notes["vocals"], 64.0, 78.0)
    w("## The vocals-stem line between 64 and 78 s")
    w("")
    if len(vn) < 3:
        w("The vocals stem has only %d notes in 64 to 78 s; no measurement." % len(vn))
    else:
        R = ds.describe_voice(clip.audio["vocals"], sr, vn, clip.stem_notes["vocals"])
        G = gs.guess(R, "vocals", "vocals", clip.prompt)
        gap = _rms(clip.audio["vocals"][int(64 * sr):int(78 * sr)].mean(axis=1)) - _rms(clip.mix[int(64 * sr):int(78 * sr)])
        w("- notes: %d in the vocals stem (basic-pitch), %d sounded alone in the stem, %d measured (%s)" % (R["n_notes"], R["alone_notes"], R["measured_n"], R["measured_on"]))
        w("- vocals stem level against the mix over 64 to 78 s: %.1f dB" % gap)
        p = R["pitch"]
        w("- pitch range: lowest %s, highest %s, median %s" % (inv.mname(p["lo"]), inv.mname(p["hi"]), inv.mname(p["median"])))
        w("- note length: %s s" % inv.fs(R["length_s"], 2))
        w("- envelope: attack %s ms; peak to sustain %s ms; sustain %s dB re peak; release %s ms" % (
            inv.fs(R["attack_s"], 0, 1000), inv.fs(R["peak_to_sus_s"], 0, 1000), inv.fs(R["sustain_db"], 1), inv.fs(R["release_s"], 0, 1000)))
        if R.get("harm_db"):
            w("- harmonics 1 to 12, median dB re harmonic 1: %s" % " ".join("%d:%.1f" % (i + 1, x) for i, x in enumerate(R["harm_db"])))
            w("- harmonic slope %s dB/octave; odd-to-even %s dB; harmonics within 30 dB %s" % (inv.fs(R["slope_db_oct"], 1), inv.fs(R["odd_even_db"], 1), inv.fs(R["n_harm_30db"], 0)))
            w("- centroid %s Hz (multiple of f0 %s); rolloff 85 percent %s Hz" % (inv.fs(R["centroid_hz"], 0), inv.fs(R["centroid_mult"], 1), inv.fs(R["rolloff_hz"], 0)))
            w("- inharmonicity %s cents; noise share %s; 2 to 5 kHz energy share %s" % (inv.fs(R["inharm_cents"], 1), inv.fs(R["noise_share"], 3), inv.fs(R["share_2_5k"], 3)))
            f = R.get("formant")
            if f:
                w("- formant-like broad peak: median %.0f Hz (p10 %.0f, p90 %.0f), n=%d notes, cv %.2f, correlation with f0 %.2f over %.1f semitones; moves with f0: %s" % (
                    f["peak_hz_median"], f["peak_hz_p10"], f["peak_hz_p90"], f["n"], f["cv"], f["corr_with_f0"], f["f0_spread_semitones"], f["moves_with_f0"]))
            else:
                w("- formant-like broad peak: not measurable (fewer than 6 alone notes with a clear peak)")
            w("- brightness (centroid Hz) 0-50 ms %s, 50-150 ms %s, 150-400 ms %s, last third %s; trend %s" % tuple(
                [inv.sv(s, 0) or "n/a" for s in R["bright_windows_hz"]] + [R["bright_trend"]]))
            w("- vibrato: share %s of %d long notes; rate %s Hz; depth %s cents; onset delay %s s" % (
                "n/a" if R["vib_share"] is None else "%.2f" % R["vib_share"], R["vib_notes_measured"], inv.fs(R["vib_rate_hz"], 1), inv.fs(R["vib_depth_cents"], 0), inv.fs(R["vib_delay_s"], 2)))
            w("- glide into notes: share %s over %d notes, cents %s, ms %s" % ("n/a" if R["glide_share"] is None else "%.2f" % R["glide_share"], R["glide_n"], inv.fs(R["glide_cents"], 0), inv.fs(R["glide_ms"], 0)))
            w("- glides between consecutive notes (portamento): share %s over %d transitions under 120 ms apart" % ("n/a" if R["portamento_share"] is None else "%.2f" % R["portamento_share"], R["portamento_n"]))
            w("- fundamental peak width above resolution: %s cents; beating %s Hz" % (inv.fs(R["fund_width_cents"], 1), inv.fs(R["beat_hz"], 1)))
        w("- side/mid of the vocals stem during these notes: %s dB" % ("n/a" if R["side_mid_db"] is None else "%.1f" % R["side_mid_db"]))
        # does the other stem carry the same pitches at the same time
        oth = clip.stem_notes["other"]
        dbl = 0
        for n in vn:
            ov = (np.minimum(n[1], oth[:, 1]) - np.maximum(n[0], oth[:, 0])) > 0.5 * (n[1] - n[0])
            if (ov & (np.abs(oth[:, 2] - n[2]) <= 1)).any():
                dbl += 1
        w("- vocals-line notes that the other stem also transcribes at the same pitch (within 1 semitone) for over half of their length: %d of %d" % (dbl, len(vn)))
        w("")
        E, st, nst = evidence(R, G)
        w("### Evidence for and against four readings")
        w("")
        w("Each line is a measured value; the thresholds are those of the guess.py rule table; a reading with more lines for than against is better supported, none is proved.")
        w("")
        for k, (A, B, U) in E.items():
            w("**%s**" % k)
            w("")
            for s in A:
                w("- for: %s" % s)
            for s in B:
                w("- against: %s" % s)
            for s in U:
                w("- not measurable or inconclusive: %s" % s)
            w("")
        w("Rule-table fractions for this line: %s." % ", ".join("%s %.2f" % x for x in G.get("all_fractions", [])) if G.get("all_fractions") else "Rule table: %s." % G["text"])
        w("")
        w("### Per-note table")
        w("")
        w("| onset s | name | length s | f0 Hz | glide cents | glide ms | vibrato Hz | vibrato cents | formant peak Hz | centroid Hz | noise share |")
        w("|---|---|---|---|---|---|---|---|---|---|---|")
        for r in R["per_note"]:
            w("| %.2f | %s | %.2f | %.0f | %.0f | %.0f | %s | %s | %s | %.0f | %.2f |" % (
                r["on"], inv.mname(r["midi"]), r["dur"], r["f0"], r["glide_c"] if np.isfinite(r["glide_c"]) else 0, r["glide_ms"] if np.isfinite(r["glide_ms"]) else 0,
                "%.1f" % r["vib"]["rate"] if r["vib"]["rate"] and np.isfinite(r["vib"]["rate"]) else "-", "%.0f" % r["vib"]["depth"] if r["vib"]["rate"] and np.isfinite(r["vib"]["rate"]) else "-",
                "%.0f" % r["fpk"] if np.isfinite(r["fpk"]) else "-", r["cen"], r["noise"] if np.isfinite(r["noise"]) else 0))
        w("")
        w("### Note sequence the line plays")
        w("")
        for ln in inv.seq_lines(vn, clip.grid):
            w(ln)
        w("")
        w("Absolute onsets, name@seconds:")
        w("")
        w(" ".join("%s@%.2f" % (inv.mname(n[2]), n[0]) for n in vn))
        w("")
        # excerpt wav
        base = os.path.join(out, "3_seoul")
        os.makedirs(base, exist_ok=True)
        inv.write_wav(os.path.join(base, "se_12_chorus_vocals_64-78.wav"), clip.audio["vocals"], sr, 64.0, 8.0)
        w("An 8 s excerpt of the vocals STEM from 64 s is in 3_seoul/se_12_chorus_vocals_64-78.wav (analysis excerpt only).")
        w("")
    with open(os.path.join(out, "se_12_chorus.md"), "w") as fh:
        fh.write("\n".join(L))
