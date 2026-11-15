#!/usr/bin/env python3
"""inventory.py - run voices.py, describe.py and guess.py over a list of (clip, window, verdict, note) and write pages.

What it does: for every row it splits the window into voices, measures each voice on its stem, guesses the instrument
family with the rule table of guess.py, and writes (all under --out):
  INDEX.md and INDEX.tsv   one row per voice (id, city, clip, window, the owner's verdict and note verbatim, stem, voice
                           rule, pitch range, attack ms, sustain dB, release ms, centroid Hz, brightness trend, vibrato Hz
                           and cents, glide share, best guess with confidence, note count, page path), sorted by city, clip,
                           window start, stem; INDEX.md opens with the voices of the windows the owner praised most strongly
                           (verdict keep and one of: excellent, phenomenal, YES, great, very good, fascinating, solid).
  <city>/<id>.md          every measurement with its count and spread, the guess with its evidence, the entrance, the
                           note sequence per bar (name:length-in-beats; by seconds when the clip's pulse strength is under
                           0.2) and the same as one line of absolute onset times.
  <city>/<id>.png         median harmonic spectrum, median amplitude envelope, centroid over time, pitch track of the three
                           longest isolated notes (under 250 kB).
  <city>/<id>.wav         up to 8 s of the STEM (not the mix), 48 kHz 16-bit, from the stretch where the voice is most
                           isolated; an analysis excerpt only.
With --chorus it also writes se_12_chorus.md (every voice between 0:50 and 1:45 of se_12 and the vocals-stem line 64-78 s).
Inputs: --list TSV (repeatable) in the windows.tsv format (file, start_s, end_s, verdict, note; file like 3_seoul/se_12_x.mp3; header
  lines start with #), --analysis DIR (the analysis folder with stems/htdemucs/<clip>, transcribe/<city>, <city>/<clip>.md),
  --clips ROOT (repeatable; mp3 roots; the file is ROOT/<file>), --prompts DIR (DIR/<city>/<clip>.txt), --out DIR,
  --sequences-for TSV (keep rows of that file also get round2_sequences/<clip>_<window>.md: per-bar bass, top line and chord notes and the drum pattern per bar),
  --override SUBSTR:min_notes=3,vocals_db=-35 (per-clip voice thresholds), --skip-verdicts drop,weak,unheard (rows with these verdicts are skipped), --only SUBSTRING (restrict to matching files).
Example: inventory.py --list windows.tsv --analysis analysis --clips /Users/alex/Dev/lyria-cities --prompts /Users/alex/Dev/lyria-cities/prompts \\
  --out analysis/instruments --chorus
Failure modes: a clip with no stems or notes is reported and skipped; the bar grid comes from the earlier DSP report and
  is laid out at the median bar length outside its tables (phase unknown there); a window with no voice writes no page;
  all measurement limits of describe.py and guess.py apply. Deterministic; no randomness.
"""
import argparse
import os
import re
import sys
import traceback

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "transcribe"))
sys.path.insert(0, HERE)
import envelopes as env  # noqa: E402
import window_report as wr  # noqa: E402
import voices as vc  # noqa: E402
import describe as ds  # noqa: E402
import guess as gs  # noqa: E402

PRAISE = re.compile(r"excellent|phenomenal|\bYES\b|great|very good|fascinating|solid", re.I)
PRAISE_RANK = [("phenomenal", 6), ("YES", 6), ("excellent", 6), ("fascinating", 5), ("great", 5), ("riveting", 5),
               ("very good", 4), ("fantastic", 5), ("very solid", 3), ("solid", 3)]
NAMES = env.NAMES


def mname(m):
    return env.midi_name(m)


def fs(s, d=2, scale=1.0, unit=""):
    """Format a stat dict as 'median x (p10 a, p90 b, n=N)'."""
    if s is None:
        return "not measurable (fewer than 5 values)"
    if s["median"] is None:
        return "not measurable (only %d values)" % s["n"]
    return "median %.*f%s (p10 %.*f, p90 %.*f, n=%d)" % (d, s["median"] * scale, unit, d, s["p10"] * scale, d, s["p90"] * scale, s["n"])


def sv(s, d=0, scale=1.0):
    return "" if s is None or s.get("median") is None else "%.*f" % (d, s["median"] * scale)


def parse_win(a, b, dur):
    ws = 0.0 if a in ("0", "") else float(a)
    we = dur if b == "end" else float(b)
    return ws, min(we, dur)


def win_label(a, b):
    return "%s-%s" % (a, b)


def short_id(clip):
    t = clip.split("_")
    return "_".join(t[:2])


class Clip:
    def __init__(self, city, name, analysis, mp3, prompt_dir):
        self.city, self.name = city, name
        d = os.path.join(analysis, "stems", "htdemucs", name)
        self.audio = {}
        self.sr = None
        for s in ("bass", "drums", "other", "vocals"):
            self.audio[s], self.sr = vc.load_stem(os.path.join(d, s + ".wav"))
        self.mix, _ = env.load_audio(mp3, self.sr)
        self.dur = len(self.mix) / self.sr
        csv = os.path.join(analysis, "transcribe", city)
        self.stem_notes = vc.clip_stem_notes(csv, name)
        self.report = os.path.join(analysis, city, name + ".md")
        self.prompt = ""
        for p in (os.path.join(prompt_dir, city, name + ".txt"),):
            if os.path.exists(p):
                self.prompt = open(p).read()
        self.mp3 = mp3
        self.grid = self._grid()

    def _grid(self):
        info = wr.parse_report(self.report, 0.0, self.dur)
        bpb = info["bpb"] or 4
        bpm = info["bpm"] or 120.0
        pulse = info["pulse"]
        starts = sorted(r[1] for r in info["bars"])
        diffs = [starts[i + 1] - starts[i] for i in range(len(starts) - 1)]
        bl0 = float(np.median(diffs)) if diffs else bpb * 60.0 / bpm
        diffs = [d for d in diffs if d < 1.5 * bl0]
        bl = float(np.median(diffs)) if diffs else bl0
        if not starts:
            starts = [0.0]
        out = list(starts)
        # fill gaps between tables, then extend to both ends of the clip
        filled = []
        for i, s in enumerate(out):
            filled.append(s)
            if i + 1 < len(out):
                t = s + bl
                while out[i + 1] - t > 0.6 * bl:
                    filled.append(t)
                    t += bl
        while filled[0] - bl >= -0.2 * bl:
            filled.insert(0, filled[0] - bl)
        while filled[-1] + bl < self.dur:
            filled.append(filled[-1] + bl)
        filled = [t for t in filled if t > -0.5 * bl]
        return dict(starts=filled, bl=bl, bpb=bpb, bpm=60.0 * bpb / bl, pulse=pulse,
                    reliable=(pulse is not None and pulse >= 0.2), from_report=bool(info["bars"]))


def praise_score(verdict, note):
    if verdict != "keep":
        return 0
    s = 0
    for w, r in PRAISE_RANK:
        if re.search(w if w != "YES" else r"\bYES\b", note, 0 if w == "YES" else re.I):
            s = max(s, r)
    return s


def seq_lines(notes, grid, drum=False):
    """Per-bar note sequence text lines for a pitched voice."""
    beat = grid["bl"] / grid["bpb"]
    starts = grid["starts"]
    L = []
    bars = {}
    for n in notes:
        k = int(np.searchsorted(starts, n[0] + 1e-6) - 1)
        bars.setdefault(max(k, 0), []).append(n)
    for k in sorted(bars):
        s = starts[k]
        if grid["reliable"]:
            lab = "bar %d (t=%.2f s)" % (k + 1, s)
            body = " ".join("%s:%s" % (mname(n[2]), wr.fmt_len(n[1] - n[0], beat, True)) for n in bars[k])
        else:
            lab = "t=%.2f-%.2f s" % (s, s + grid["bl"])
            body = " ".join("%s:%.2fs" % (mname(n[2]), n[1] - n[0]) for n in bars[k])
        L.append("- %s: %s" % (lab, body))
    return L


def best_excerpt(v, clip, stem):
    """Start time of the 8 s stretch where the voice is most isolated in its stem."""
    notes = v["notes"]
    t0, t1 = notes[:, 0].min(), notes[:, 1].max()
    others = clip.stem_notes.get(stem, np.zeros((0, 4)))
    step = 0.5
    best, bs = t0, -1e9
    t = max(0.0, t0 - 0.0)
    L = min(8.0, clip.dur)
    while t + L <= min(clip.dur, max(t1, t0 + L)) + 1e-9:
        mine = sum(max(0, min(n[1], t + L) - max(n[0], t)) for n in notes)
        allm = sum(max(0, min(n[1], t + L) - max(n[0], t)) for n in others)
        sc = mine - 2.0 * max(0.0, allm - mine)
        if sc > bs:
            bs, best = sc, t
        t += step
    return best, L


def write_wav(path, x, sr, t0, L):
    from scipy import signal
    from scipy.io import wavfile
    seg = x[int(t0 * sr):int((t0 + L) * sr)].astype(np.float64)
    y = signal.resample_poly(seg, 160, 147, axis=0) if sr == 44100 else seg
    pk = np.abs(y).max()
    if pk > 0.99:
        y = y * (0.99 / pk)
    wavfile.write(path, 48000, np.clip(np.round(y * 32767), -32768, 32767).astype(np.int16))


def write_png(path, R, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(2, 2, figsize=(8, 5))
    a = ax[0, 0]
    if R.get("harm_db"):
        a.bar(range(1, 13), R["harm_db"], color="#446")
        a.set_ylim(-60, 3)
    a.set_title("median harmonic spectrum (dB re h1)", fontsize=8)
    a.set_xlabel("harmonic", fontsize=7)
    a = ax[0, 1]
    if R.get("median_env_db") is not None:
        a.plot(ds.GRID_T, R["median_env_db"], color="#a43")
        a.set_ylim(-40, 3)
    a.set_title("median amplitude envelope (dB re peak)", fontsize=8)
    a.set_xlabel("s from note start", fontsize=7)
    a = ax[1, 0]
    if R.get("median_centroid_hz") is not None:
        a.plot(ds.GRID_T, R["median_centroid_hz"], color="#385")
    a.set_title("median centroid (Hz) over time", fontsize=8)
    a.set_xlabel("s from note start", fontsize=7)
    a = ax[1, 1]
    for tt, cc, midi, dur in R.get("pitch_tracks", []):
        ok = np.isfinite(cc)
        a.plot(tt[ok], cc[ok], label=mname(midi), lw=1)
    a.set_ylim(-150, 150)
    if R.get("pitch_tracks"):
        a.legend(fontsize=6)
    a.set_title("pitch track, 3 longest notes (cents re settled)", fontsize=8)
    a.set_xlabel("s from note start", fontsize=7)
    for r in ax.flat:
        r.tick_params(labelsize=6)
    fig.suptitle(title, fontsize=8)
    fig.tight_layout()
    for dpi in (70, 55, 40):
        fig.savefig(path, dpi=dpi)
        if os.path.getsize(path) < 250000:
            break
    plt.close(fig)


def write_drum_png(path, R, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(1, 2, figsize=(8, 3))
    if R.get("median_spec_db") is not None:
        ax[0].semilogx(R["spec_freqs"], R["median_spec_db"], color="#446")
        ax[0].set_ylim(-70, 3)
        ax[1].plot(ds.GRID_T, R["median_env_db"], color="#a43")
        ax[1].set_xlim(0, 0.5)
        ax[1].set_ylim(-60, 3)
    ax[0].set_title("median spectrum of the first 100 ms (dB re peak)", fontsize=8)
    ax[1].set_title("median amplitude envelope (dB re peak)", fontsize=8)
    for r in ax:
        r.tick_params(labelsize=6)
    fig.suptitle(title, fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=70)
    plt.close(fig)


def page_text(vid, meta, v, R, G, clip, grid, rel_png, rel_wav, exc):
    L = []
    w = L.append
    w("# %s" % vid)
    w("")
    w("All values are measurements of a separated stem at transcribed note times; nothing here is a listening result.")
    w("")
    w("- clip: %s/%s" % (meta["city"], meta["clip"]))
    w("- window: %s to %s s" % (meta["ws"], meta["we"]))
    w("- the owner's verdict for this window: %s" % meta["verdict"])
    w("- the owner's note, verbatim: %s" % meta["note"])
    w("- stem: %s" % v["stem"])
    w("- voice rule: %s" % v["rule"])
    w("- excerpt of the stem (not the mix): %s (%.1f s from %.1f s)" % (rel_wav, exc[1], exc[0]))
    w("- plot: %s" % rel_png)
    w("")
    w("## Best guess")
    w("")
    w("- guess: %s" % G["text"])
    w("- confidence: %s" % G["confidence"])
    w("- evidence (+ rule met, - rule not met): %s" % G["evidence"])
    if G.get("all_fractions"):
        w("- rule fractions: %s" % ", ".join("%s %.2f" % (f, x) for f, x in G["all_fractions"]))
    if G.get("prompt_asked"):
        w("- the prompt asked for (words in the prompt text, not heard): %s" % ", ".join(G["prompt_asked"]))
    w("")
    if R["kind"] == "drum":
        w("## Drum measurements")
        w("")
        w("- onsets: %d" % R["n_notes"])
        w("- decay time to -20 dB: %s s" % fs(R["decay_20db_s"], 3))
        w("- centroid: %s Hz" % fs(R["centroid_hz"], 0))
        w("- peak frequency: %s Hz" % fs(R["peak_hz"], 0))
        w("- pitch drop over the first 50 ms: %s Hz" % fs(R["pitch_drop_hz"], 0))
        w("- noise share: %s" % fs(R["noise_share"], 3))
        if R["pattern"]:
            p = R["pattern"]
            w("- pattern per bar, %d steps, x = onset in half or more of the %d bars, o = in a quarter or more: `%s`" % (p["steps"], p["bars"], p["line"]))
            w("- onset share per step: %s" % " ".join("%.2f" % s for s in p["share"]))
        else:
            w("- pattern: not available (the clip's pulse strength is under 0.2 or the grid is missing)")
    else:
        w("## Notes")
        w("")
        w("- note count: %d, measured: %d (%s); %d of the voice's notes sounded alone in their stem" % (R["n_notes"], R["measured_n"], R["measured_on"], R["alone_notes"]))
        if R["dropped"]:
            w("- notes dropped from measurement: %s" % ", ".join("%s %d" % (k, c) for k, c in R["dropped"].items()))
        p = R["pitch"]
        w("- pitch range: lowest %s, highest %s, median %s" % (mname(p["lo"]), mname(p["hi"]), mname(p["median"])))
        w("- note length: %s s" % fs(R["length_s"], 2))
        w("- polyphony of the voice at its onsets: median %.1f; of the whole stem: median %.1f" % (R["polyphony_median"], R["stem_polyphony_median"]))
        w("")
        w("## Envelope (envelopes.py on the stem, measured notes of 0.12 s or more, n=%d)" % R["env_n"])
        w("")
        w("- attack 10 to 90 percent: %s ms" % fs(R["attack_s"], 0, 1000))
        w("- time from peak to sustain: %s ms" % fs(R["peak_to_sus_s"], 0, 1000))
        w("- sustain level re peak: %s dB" % fs(R["sustain_db"], 1))
        w("- release (20 dB under sustain after the note end): %s ms" % fs(R["release_s"], 0, 1000))
        if R.get("harm_db"):
            w("")
            w("## Spectral shape (middle half of each note)")
            w("")
            w("- harmonics 1 to 12, median dB re harmonic 1: %s" % " ".join("%d:%.1f" % (i + 1, x) for i, x in enumerate(R["harm_db"])))
            w("- harmonics 1 to 12, p10 to p90 dB: %s" % " ".join("%d:%.0f..%.0f" % (i + 1, a, b) for i, (a, b) in enumerate(zip(R["harm_db_p10"], R["harm_db_p90"]))))
            w("- harmonic slope: %s dB per octave" % fs(R["slope_db_oct"], 1))
            w("- harmonics within 30 dB of harmonic 1: %s" % fs(R["n_harm_30db"], 0))
            w("- odd-to-even ratio (harmonics 3 to 11 over 2 to 12): %s dB" % fs(R["odd_even_db"], 1))
            w("- spectral centroid: %s Hz; as a multiple of f0: %s" % (fs(R["centroid_hz"], 0), fs(R["centroid_mult"], 1)))
            w("- rolloff 85 percent: %s Hz" % fs(R["rolloff_hz"], 0))
            w("- inharmonicity (partials 2 to 8, cents): %s; correlation with pitch %s" % (fs(R["inharm_cents"], 1), "n/a" if R.get("inharm_pitch_corr") is None else "%.2f" % R["inharm_pitch_corr"]))
            w("- noise share (energy not within 40 cents of a harmonic): %s" % fs(R["noise_share"], 3))
            w("- share of energy at 2 to 5 kHz: %s" % fs(R["share_2_5k"], 3))
            if R.get("formant"):
                f = R["formant"]
                w("- formant-like broad peak of the harmonic envelope (350 Hz Gaussian smoothing): median %.0f Hz (p10 %.0f, p90 %.0f), n=%d notes, cv %.2f, correlation with f0 %s over a f0 spread of %.1f semitones, moves with f0: %s" % (
                    f["peak_hz_median"], f["peak_hz_p10"], f["peak_hz_p90"], f["n"], f["cv"], "n/a" if not np.isfinite(f["corr_with_f0"]) else "%.2f" % f["corr_with_f0"], f["f0_spread_semitones"], f["moves_with_f0"]))
            else:
                w("- formant-like peak: not measurable (fewer than 6 alone notes with a clear envelope peak)")
            w("")
            w("## Brightness over time (centroid, Hz)")
            w("")
            bw = R["bright_windows_hz"]
            for lab, s in zip(("0-50 ms", "50-150 ms", "150-400 ms", "last third"), bw):
                w("- %s: %s" % (lab, fs(s, 0)))
            w("- last third over 50-150 ms: %s" % fs(R["bright_last_over_50_150"], 2))
            w("- trend: %s (counts %s; falls under 0.85, rises over 1.18)" % (R["bright_trend"] or "not measurable", R["bright_trend_counts"]))
            w("- centroid against level correlation within a note: %s" % fs(R["bright_level_corr"], 2))
            w("")
            w("## Pitch behaviour")
            w("")
            w("- vibrato: share of notes of 0.6 s or more with vibrato %s over %d notes; rate %s Hz; depth (amplitude, cents) %s; onset delay %s s" % (
                "n/a" if R["vib_share"] is None else "%.2f" % R["vib_share"], R["vib_notes_measured"], fs(R["vib_rate_hz"], 1), fs(R["vib_depth_cents"], 0), fs(R["vib_delay_s"], 2)))
            w("- glide into notes: share %s over %d notes; cents %s; ms %s" % ("n/a" if R["glide_share"] is None else "%.2f" % R["glide_share"], R["glide_n"], fs(R["glide_cents"], 0), fs(R["glide_ms"], 0)))
            w("- portamento share between consecutive notes under 120 ms apart: %s over %d transitions" % ("n/a" if R["portamento_share"] is None else "%.2f" % R["portamento_share"], R["portamento_n"]))
            w("- fundamental peak width above the window's own resolution (cents): %s" % fs(R["fund_width_cents"], 1))
            w("- beating (amplitude modulation under 12 Hz): %s Hz; found in share %s of %d notes of 0.8 s or more" % (fs(R["beat_hz"], 1), "n/a" if R["beat_share"] is None else "%.2f" % R["beat_share"], R["beat_notes_measured"]))
    w("")
    w("## Stereo and entrance")
    w("")
    if R.get("side_mid_db") is not None:
        w("- side/mid ratio of the stem during the voice's notes: %.1f dB" % R["side_mid_db"])
    E = R["entrance"]
    w("- first note of the voice in the window: %.2f s" % E["t"])
    if E.get("pre_silent") is None:
        w("- the 2 s before it: %s" % E.get("pre_note", "not measurable"))
    else:
        w("- the %.1f s before it are %s in this stem (%.1f dB against %.1f dB in the 4 s after)" % (E["pre_len_s"], "silent" if E["pre_silent"] else "not silent", E["pre_db"], E["after_db"]))
    w("- first 2 s: level change %s dB, fade in: %s; centroid ratio last over first quarter-second %s, filter opening: %s" % (
        "n/a" if "level_rise_db" not in E else "%.1f" % E["level_rise_db"], E.get("fade_in"), "n/a" if "centroid_rise" not in E else "%.2f" % E["centroid_rise"], E.get("filter_opening")))
    w("")
    w("## Note sequence")
    w("")
    if R["kind"] == "drum":
        w("Onset times in seconds: " + " ".join("%.2f" % t for t in v["notes"][:, 0]))
    else:
        if not grid["reliable"]:
            w("The clip's pulse strength is under 0.2 or missing, so bars are not used; lines are by seconds (the bar length of the grid is shown).")
        else:
            w("Bars come from the earlier DSP report grid; lengths are in beats (quarter-beat rounding).")
        w("")
        for ln in seq_lines(v["notes"], grid):
            w(ln)
        w("")
        w("Absolute onsets, name@seconds:")
        w("")
        w(" ".join("%s@%.2f" % (mname(n[2]), n[0]) for n in v["notes"]))
    w("")
    return "\n".join(L)


def index_row(vid, meta, v, R, G, page):
    if R["kind"] == "drum":
        pr = "drum onsets %d" % R["n_notes"]
        att = sus = rel = cen = trend = vib = glide = ""
        cen = sv(R["centroid_hz"], 0)
        rel = ""
        att = "decay20 %s ms" % sv(R["decay_20db_s"], 0, 1000)
    else:
        p = R["pitch"]
        pr = "%s-%s (median %s)" % (mname(p["lo"]), mname(p["hi"]), mname(p["median"]))
        att, sus, rel = sv(R["attack_s"], 0, 1000), sv(R["sustain_db"], 1), sv(R["release_s"], 0, 1000)
        cen = sv(R.get("centroid_hz"), 0)
        trend = R.get("bright_trend") or ""
        vr_ = R.get("vib_rate_hz")
        vib = "%s Hz %s cents" % (sv(vr_, 1), sv(R["vib_depth_cents"], 0)) if vr_ and vr_.get("median") is not None else ""
        if R.get("vib_share") is not None and not vib:
            vib = "none (share %.2f)" % R["vib_share"]
        glide = "" if R.get("glide_share") is None else "%.2f" % R["glide_share"]
    return [vid, meta["city"], meta["clip"], meta["label"], meta["verdict"], meta["note"], v["stem"], v["rule"], pr, att, sus, rel,
            cen, trend, vib, glide, "%s (%s)" % (G["text"], G["confidence"]), str(R["n_notes"]), page]


COLS = ["id", "city", "clip", "window", "owner verdict", "owner note", "stem", "voice rule", "pitch range", "attack ms",
        "sustain dB", "release ms", "centroid Hz", "brightness trend", "vibrato", "glide share", "best guess (confidence)",
        "note count", "page"]


def read_rows(path):
    rows = []
    for ln in open(path):
        if ln.startswith("#") or not ln.strip():
            continue
        f = ln.rstrip("\n").split("\t")
        f += [""] * (5 - len(f))
        rows.append(f[:5])
    return rows


OVERRIDES = {}


def run_voices(clip, ws, we):
    bpm = clip.grid["bpm"] if clip.grid["bpm"] else 120.0
    kw = {}
    for sub, d in OVERRIDES.items():
        if sub in clip.name:
            kw.update(d)
    return vc.split_all(clip.stem_notes, clip.audio, clip.sr, clip.mix, ws, we, bpm, **kw)


def round2_sequences(clip, ws, we, row, vs, path):
    """Whole-window per-bar sequences of the bass, the top line and the chord notes, and the drum pattern per bar."""
    g = clip.grid
    bl = g["bl"]
    starts = [s for s in g["starts"] if ws - 0.3 * bl <= s < we]
    L = ["# %s window %s-%s: sequences for re-scoring" % (clip.name, row[1], row[2]), "",
         "- the owner's verdict: %s" % row[3], "- the owner's note, verbatim: %s" % row[4],
         "- bar grid: %s (bar length %.3f s, %d beats per bar, pulse strength %s)" % (
             "usable" if g["reliable"] else "UNRELIABLE, bars are laid at the median bar length and labelled by seconds",
             bl, g["bpb"], "n/a" if g["pulse"] is None else "%.2f" % g["pulse"]),
         "- lengths are in beats when the grid is usable, else in seconds; notes are listed by bar of onset", ""]
    names = {"bass": "bass", "top": "top line (other stem)", "chords": "chords or pad notes (other stem)",
             "short": "short notes (other stem)", "lowmid": "low-mid line (other stem)", "vocals": "vocals-stem voice"}
    for v in vs:
        if v["kind"] != "pitched":
            continue
        L += ["## %s (voice %s, stem %s, %d notes)" % (names.get(v["name"], v["name"]), v["name"], v["stem"], len(v["notes"])), ""]
        L += seq_lines(v["notes"], g) + [""]
        if v["name"] == "chords":
            L += ["Pitch classes sounding per bar from these notes:", ""]
            for k, s in enumerate(starts):
                pcs = sorted({int(n[2]) % 12 for n in v["notes"] if n[0] < s + bl and n[1] > s + 0.1 * bl})
                if pcs:
                    L.append("- bar %d (t=%.2f s): %s" % (k + 1, s, " ".join(NAMES[p] for p in pcs)))
            L.append("")
    dv = [v for v in vs if v["kind"] == "drum"]
    if dv:
        L += ["## Drum pattern per bar (16 steps for 4 beats, 12 for 3; x = onset)", ""]
        steps = 16 if g["bpb"] == 4 else 12
        for k, s in enumerate(starts):
            cells = []
            for v in dv:
                ch = ["."] * steps
                for t in v["notes"][:, 0]:
                    if s - 0.02 <= t < s + bl - 0.02:
                        ch[min(steps - 1, int(round((t - s) / bl * steps)) % steps)] = "x"
                cells.append("%s %s" % (v["name"].replace("drums_", ""), "".join(ch)))
            L.append("- bar %d (t=%.2f s): %s" % (k + 1, s, " | ".join(cells)))
        L.append("")
    with open(path, "w") as fh:
        fh.write("\n".join(L))


def process(clip, row, out, analysis, index, seq_files=()):
    f, a, b, verdict, note = row
    city = os.path.dirname(f)
    ws, we = parse_win(a, b, clip.dur)
    label = win_label(a, b)
    vs, left = run_voices(clip, ws, we)
    os.makedirs(os.path.join(out, city), exist_ok=True)
    if f in seq_files and verdict == "keep":
        d = os.path.join(out, "round2_sequences")
        os.makedirs(d, exist_ok=True)
        round2_sequences(clip, ws, we, row, vs, os.path.join(d, "%s_%s.md" % (clip.name, label)))
    n_written = 0
    for v in vs:
        vid = "%s_%s_%s_%s" % (short_id(clip.name), label, v["stem"], v["name"] if v["name"] not in ("bass", "vocals") else v["name"])
        vid = vid.replace("_bass_bass", "_bass").replace("_vocals_vocals", "_vocals")
        if v["kind"] == "drum":
            vid = "%s_%s_%s" % (short_id(clip.name), label, v["name"])
        stem_notes = clip.stem_notes.get(v["stem"], np.zeros((0, 4)))
        if v["kind"] == "pitched":
            R = ds.describe_voice(clip.audio[v["stem"]], clip.sr, v["notes"], stem_notes)
        else:
            g = None
            if clip.grid["reliable"]:
                st = [s for s in clip.grid["starts"] if ws - 0.01 <= s < we]
                g = (st, clip.grid["bl"], clip.grid["bpb"])
            R = ds.describe_drum(clip.audio["drums"], clip.sr, list(v["notes"][:, 0]), v["name"], g)
            R["entrance"] = ds.entrance(clip.audio["drums"].mean(axis=1), clip.sr, float(v["notes"][0, 0]), v["notes"])
        G = gs.guess(R, v["stem"], v["name"], clip.prompt)
        base = os.path.join(out, city, vid)
        exc = best_excerpt(v, clip, v["stem"]) if v["kind"] == "pitched" else (max(0.0, float(v["notes"][0, 0])), min(8.0, clip.dur))
        write_wav(base + ".wav", clip.audio[v["stem"]], clip.sr, exc[0], exc[1])
        if R["kind"] == "pitched":
            write_png(base + ".png", R, vid)
        else:
            write_drum_png(base + ".png", R, vid)
        meta = dict(city=city, clip=clip.name, ws="%.2f" % ws, we="%.2f" % we, verdict=verdict, note=note, label=label)
        txt = page_text(vid, meta, v, R, G, clip, clip.grid, vid + ".png", vid + ".wav", exc)
        with open(base + ".md", "w") as fh:
            fh.write(txt)
        index.append((city, clip.name, ws, v["stem"], index_row(vid, meta, v, R, G, base + ".md"), verdict, note, G, vid, label))
        n_written += 1
    return n_written, left


def write_index(out, index):
    index.sort(key=lambda r: (r[0], r[1], r[2], r[3], r[8]))
    with open(os.path.join(out, "INDEX.tsv"), "w") as fh:
        fh.write("\t".join(COLS) + "\n")
        for r in index:
            fh.write("\t".join(c.replace("\t", " ").replace("\n", " ") for c in r[4]) + "\n")
    L = []
    w = L.append
    w("# Instrument inventory")
    w("")
    w("One row per voice.")
    w("Everything is a measurement of separated stems at transcribed note times; guesses come from the rule table in guess.py and are not listening results.")
    w("The columns are in INDEX.tsv; this page lists the voice ids per window with the guess.")
    w("")
    w("## Voices in the windows the owner praised most strongly")
    w("")
    w("Windows with verdict keep and one of: excellent, phenomenal, YES, great, very good, fascinating, solid.")
    w("")
    praised = {}
    for r in index:
        if praise_score(r[5], r[6]) > 0:
            praised.setdefault((r[0], r[1], r[9], r[5], r[6]), []).append(r)
    for k in sorted(praised, key=lambda k: (-max(praise_score(k[3], k[4]), 0), k[0], k[1], k[2])):
        w("- %s %s window %s (owner: %s, \"%s\")" % (k[0], k[1], k[2], k[3], k[4]))
        for r in praised[k]:
            w("  - %s: %s (%s), %s notes" % (r[8], r[7]["text"], r[7]["confidence"], r[4][17]))
    w("")
    w("## All voices")
    w("")
    cur = None
    for r in index:
        if (r[0], r[1]) != cur:
            cur = (r[0], r[1])
            w("")
            w("### %s %s" % cur)
            w("")
        w("- %s [%s, owner: %s] %s (%s), %s notes: %s" % (r[8], r[9], r[5], r[7]["text"], r[7]["confidence"], r[4][17], r[4][7]))
    with open(os.path.join(out, "INDEX.md"), "w") as fh:
        fh.write("\n".join(L) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--list", action="append", required=True, help="windows TSV; repeatable, rows are processed in the order given")
    ap.add_argument("--sequences-for", help="TSV (windows format): its keep rows also get a whole-window round2_sequences page")
    ap.add_argument("--override", action="append", default=[], help="SUBSTR:min_notes=3,vocals_db=-35 for clips whose name has SUBSTR")
    ap.add_argument("--analysis", required=True)
    ap.add_argument("--clips", action="append", required=True)
    ap.add_argument("--prompts", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--skip-verdicts", default="drop,weak,unheard")
    ap.add_argument("--only")
    ap.add_argument("--chorus", action="store_true")
    ap.add_argument("--chorus-only", action="store_true")
    a = ap.parse_args()
    skip = set(a.skip_verdicts.split(",")) if a.skip_verdicts else set()
    for o in a.override:
        sub, kv = o.split(":")
        OVERRIDES[sub] = {k: (int(x) if k == "min_notes" else float(x)) for k, x in (p.split("=") for p in kv.split(","))}
    seq_files = set(r[0] for r in read_rows(a.sequences_for)) if a.sequences_for else set()
    rows = [r for lst in a.list for r in read_rows(lst)]
    rows = [r for r in rows if r[3] not in skip and (not a.only or a.only in r[0])]
    os.makedirs(a.out, exist_ok=True)
    byclip = {}
    for r in rows:
        byclip.setdefault(r[0], []).append(r)
    order = []
    for r in rows:
        if r[0] not in order:
            order.append(r[0])
    index, problems = [], []
    if not a.chorus_only:
        for f in order:
            city = os.path.dirname(f)
            name = os.path.splitext(os.path.basename(f))[0]
            mp3 = next((os.path.join(r_, f) for r_ in a.clips if os.path.exists(os.path.join(r_, f))), None)
            if mp3 is None:
                problems.append("missing mp3 %s" % f)
                continue
            try:
                clip = Clip(city, name, a.analysis, mp3, a.prompts)
            except Exception as ex:
                problems.append("cannot load %s: %r" % (f, ex))
                continue
            for r in byclip[f]:
                try:
                    n, left = process(clip, r, a.out, a.analysis, index, seq_files)
                    print("%s %s-%s: %d voices; %s" % (name, r[1], r[2], n, "; ".join("%s" % v for v in left.values())), flush=True)
                except Exception:
                    traceback.print_exc()
                    problems.append("failed %s %s-%s" % (f, r[1], r[2]))
        write_index(a.out, index)
    if a.chorus or a.chorus_only:
        import chorus
        chorus.write_chorus(a, sys.modules[__name__])
    for p in problems:
        print("PROBLEM", p)
    print("voices:", len(index))


if __name__ == "__main__":
    main()
