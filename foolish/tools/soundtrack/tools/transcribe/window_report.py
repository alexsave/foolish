#!/usr/bin/env python3
"""window_report.py - per-window re-scoring report (Markdown plus raw-note TSV) from stems, note CSVs and a bar grid.

What it does: for one time window of one clip it writes, as measurements only (no listening verdicts):
  bass line per bar (with the earlier DSP bass estimate and agreement), melody/top line per bar from the "other"
  stem (and from the vocals stem when it is within 12 dB of "other" in the window), chords per bar, a drum pattern
  per bar (low/mid/high, 16 steps per 4-beat bar, 12 per 3-beat bar), the vocals-stem level against the mix over
  the whole file, note envelopes (via envelopes.py) and confidence numbers.
Inputs (all paths are arguments):
  --name CLIP --mix MIXFILE --start S --end E|end
  --stems-dir DIR (holding bass.wav drums.wav other.wav vocals.wav)
  --csv-mix/--csv-bass/--csv-other/--csv-vocals note CSVs from notes.sh (only the first five columns are read)
  --report-md earlier DSP report (the <clip>.md with the "Per-bar table" and "- pulse_strength:" lines); gives the
      bar start times, beats per bar, pulse strength and DSP bass. Without it use --bpm, --beats-per-bar, --first-beat.
  --prompt prompt text file (keys such as "G minor" are read from it), --index INDEX.tsv (a row is added or replaced)
  --out-md --out-tsv outputs. --pulse-min 0.2: below it the grid is called unreliable and times are given in seconds.
Example: tools/transcribe/window_report.py --name mo_03 --mix mo_03.mp3 --stems-dir stems/htdemucs/mo_03 \\
  --csv-mix mo_03_basic_pitch.csv --csv-bass mo_03_bass_basic_pitch.csv --csv-other mo_03_other_basic_pitch.csv \\
  --csv-vocals mo_03_vocals_basic_pitch.csv --report-md mo_03.md --start 0 --end end --out-md out.md --out-tsv out.tsv
Known failure modes: bars come from the earlier beat tracker (wrong phase, half or double time, meters other than
  3 or 4 are forced); basic-pitch ghost notes and missing notes pass straight through; the drum classes are
  per-onset spectral-flux shares, so a hat sounding with a kick is flagged only when its high share is at least
  0.35, and a kick click can leak into the mid row; chord labels are template matches over at most 5 types of
  triad and seventh chords; voice-stem level only says what the separator put in that stem; all analysis is
  deterministic, no randomness anywhere.
"""
import argparse
import collections
import os
import re
import sys

import numpy as np
from scipy import signal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import envelopes as env  # noqa: E402

NAMES = env.NAMES
midi_name = env.midi_name
MINOR_KS = np.array([6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17])
MAJOR_KS = np.array([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88])
CHORDS = [("", (0, 4, 7)), ("m", (0, 3, 7)), ("dim", (0, 3, 6)), ("aug", (0, 4, 8)),
          ("maj7", (0, 4, 7, 11)), ("7", (0, 4, 7, 10)), ("m7", (0, 3, 7, 10)),
          ("m7b5", (0, 3, 6, 10)), ("dim7", (0, 3, 6, 9))]
MIN_NOTE = 0.06


def pc_of_name(s):
    base = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}[s[0].upper()]
    for ch in s[1:]:
        if ch in "#♯":
            base += 1
        elif ch in "b♭":
            base -= 1
    return base % 12


def parse_time(s):
    m, r = s.split(":")
    return int(m) * 60 + float(r)


def parse_report(path, ws, we):
    """Pulse, beats per bar, DSP bar table [(bar, start, bass_name_or_None)] of the table matching the window."""
    info = {"pulse": None, "bpb": None, "bpm": None, "bars": [], "sections": []}
    if not path or not os.path.exists(path):
        return info
    lines = open(path).read().split("\n")
    tables, cur = [], None
    for ln in lines:
        m = re.match(r"- pulse_strength: ([0-9.]+)", ln)
        if m:
            info["pulse"] = float(m.group(1))
        m = re.match(r"- beats_per_bar: (\d)", ln)
        if m:
            info["bpb"] = int(m.group(1))
        m = re.match(r"- BPM: ([0-9.]+)", ln)
        if m:
            info["bpm"] = float(m.group(1))
        m = re.match(r"## Per-bar table (\d+:[0-9.]+)-(\d+:[0-9.]+)", ln)
        if m:
            cur = {"a": parse_time(m.group(1)), "b": parse_time(m.group(2)), "rows": []}
            tables.append(cur)
            continue
        if ln.startswith("## "):
            cur = None
        if cur is not None and re.match(r"\| \d+ \|", ln):
            c = [x.strip() for x in ln.strip("|").split("|")]
            bass = c[-4].split()[0] if c[-4] and c[-4] != "-" else None  # chord cell may contain "|"
            cur["rows"].append((int(c[0]), float(c[1]), bass))
    merged = {}
    for t in tables:
        if t["b"] > ws and (we is None or t["a"] < we):
            for r in t["rows"]:
                merged[round(r[1], 1)] = r
    info["bars"] = [merged[k] for k in sorted(merged)]
    info["sections"] = parse_sections(lines)
    return info


def parse_sections(lines):
    """[(start, end, tempo)] from the earlier report's "## Sections" table."""
    out, on = [], False
    for ln in lines:
        if ln.startswith("## "):
            on = ln.startswith("## Sections")
            continue
        if on and re.match(r"\| \d+ \|", ln):
            c = [x.strip() for x in ln.strip("|").split("|")]
            out.append((parse_time(c[1]), parse_time(c[2]), c[5]))
    return out


def note_pc(name):
    return pc_of_name(re.match(r"[A-G][#b]?", name).group(0))


def fmt_len(dur, beat, beats_mode):
    if beats_mode:
        q = max(0.25, round(dur / beat * 4) / 4.0)
        return ("%g" % q)
    return "%.2fs" % dur


def lines_of(notes, t0, t1, dt=0.01):
    """Top line and second line (highest sounding pitch at least 3 semitones below) as per-frame midi arrays."""
    n = int(round((t1 - t0) / dt))
    top = np.full(n, -1)
    sec = np.full(n, -1)
    act = [[] for _ in range(n)]
    for on, off, m, v in notes:
        if off - on < MIN_NOTE:
            continue
        a, b = max(0, int((on - t0) / dt)), min(n, int(np.ceil((off - t0) / dt)))
        for i in range(a, b):
            act[i].append(int(m))
    for i, l in enumerate(act):
        if l:
            top[i] = max(l)
            low = [p for p in l if p <= top[i] - 3]
            if low:
                sec[i] = max(low)
    return top, sec


def segments(arr, t0, dt=0.01, min_len=MIN_NOTE):
    out, i, n = [], 0, len(arr)
    while i < n:
        j = i
        while j < n and arr[j] == arr[i]:
            j += 1
        if arr[i] >= 0 and (j - i) * dt >= min_len:
            out.append((t0 + i * dt, (j - i) * dt, int(arr[i])))
        i = j
    return out


def chord_of(notes, a, b):
    occ = np.zeros(12)
    en = np.zeros(12)
    nfr = 200
    frames = np.zeros((12, nfr), bool)
    for on, off, m, v in notes:
        if off - on < MIN_NOTE:
            continue
        o = min(off, b) - max(on, a)
        if o <= 0:
            continue
        pc = int(m) % 12
        en[pc] += o * v
        i0 = int((max(on, a) - a) / (b - a) * nfr)
        i1 = int(np.ceil((min(off, b) - a) / (b - a) * nfr))
        frames[pc, i0:i1] = True
    occ = frames.mean(axis=1)
    sounding = {p for p in range(12) if occ[p] >= 0.25}
    if not sounding or en.sum() <= 0:
        return [], "-", 0.0
    best = None
    for root in range(12):
        for suf, iv in CHORDS:
            tones = {(root + i) % 12 for i in iv}
            expl = sum(en[p] for p in tones) / en.sum()
            miss = len(tones - sounding)
            sc = expl - 0.04 * (len(iv) - 3) - 0.10 * miss
            if best is None or sc > best[0] + 1e-12:
                best = (sc, root, suf, expl)
    return sorted(sounding), NAMES[best[1]] + best[2], best[3]


def drum_onsets(x, sr, t0, t1):
    n, hop = 1024, 256
    a, b = max(0, int((t0 - 0.1) * sr)), min(len(x), int((t1 + 0.1) * sr))
    seg = x[a:b].astype(np.float64)
    if len(seg) < n * 2 or np.sqrt(np.mean(seg ** 2) + 1e-20) < 1e-4:
        return []
    win = np.hanning(n)
    nf = 1 + (len(seg) - n) // hop
    idx = np.arange(n)[None, :] + hop * np.arange(nf)[:, None]
    mag = np.abs(np.fft.rfft(seg[idx] * win, axis=1))
    X = np.log1p(mag * (30.0 / (mag.max() + 1e-12)))
    fr = np.fft.rfftfreq(n, 1.0 / sr)
    d = np.maximum(0, np.diff(X, axis=0, prepend=X[:1]))
    bands = [fr < 150, (fr >= 150) & (fr <= 2000), fr > 5000]
    F = d.sum(axis=1)
    Fs = signal.savgol_filter(F, 5, 2) if len(F) > 5 else F
    p95 = np.percentile(Fs, 95)
    if p95 <= 0:
        return []
    pw = mag ** 2
    dp = np.maximum(0, np.diff(pw, axis=0, prepend=pw[:1]))
    pk, _ = signal.find_peaks(Fs, height=0.3 * p95, distance=int(0.035 * sr / hop), prominence=0.12 * p95)
    out = []
    for p in pk:
        g = [dp[max(0, p - 1):p + 10][:, m].sum() for m in bands]
        tot = sum(g) + 1e-30
        sl, sm, sh = g[0] / tot, g[1] / tot, g[2] / tot
        cls = []
        if sl > 0.5:
            cls.append("L")
            if sh >= 0.35:
                cls.append("H")
        elif sh > sm:
            cls.append("H")
        else:
            cls.append("M")
        t = (a + p * hop + n / 2) / sr
        if t0 - 0.1 <= t < t1 + 0.1:
            out.append((t, cls))
    return out


def rms_db(x):
    return 20 * np.log10(np.sqrt(np.mean(x.astype(np.float64) ** 2) + 1e-20))


def keys_from_prompt(path):
    if not path or not os.path.exists(path):
        return []
    txt = open(path).read()
    ks = []
    for m in re.finditer(r"\b([A-G])(?:[ -]?(sharp|flat)|([#b♯♭]))?[ -]+(major|minor)\b", txt, re.I):
        nm = m.group(1).upper() + ({"sharp": "#", "flat": "b"}.get((m.group(2) or "").lower(), m.group(3) or ""))
        k = (nm, m.group(4).lower())
        if k not in ks:
            ks.append(k)
    return ks


def key_corr(notes, keys):
    h = np.zeros(12)
    for on, off, m, v in notes:
        h[int(m) % 12] += off - on
    best = None
    for nm, mode in keys:
        r = pc_of_name(nm)
        prof = np.roll(MINOR_KS if mode == "minor" else MAJOR_KS, r)
        if h.std() == 0:
            c = float("nan")
        else:
            c = float(np.corrcoef(h, prof)[0, 1])
        if best is None or (c == c and (best[0] != best[0] or c > best[0])):
            best = (c, "%s %s" % (nm, mode))
    return best


def sel(notes, ws, we):
    if len(notes) == 0:
        return notes
    return notes[(notes[:, 0] >= ws) & (notes[:, 0] < we)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--name", required=True)
    ap.add_argument("--mix", required=True)
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--end", default="end")
    ap.add_argument("--stems-dir", required=True)
    for k in ("mix", "bass", "other", "vocals"):
        ap.add_argument("--csv-" + k)
    ap.add_argument("--report-md")
    ap.add_argument("--bpm", type=float)
    ap.add_argument("--beats-per-bar", type=int, default=4)
    ap.add_argument("--first-beat", type=float)
    ap.add_argument("--pulse-min", type=float, default=0.2)
    ap.add_argument("--prompt")
    ap.add_argument("--index")
    ap.add_argument("--out-md", required=True)
    ap.add_argument("--out-tsv", required=True)
    ap.add_argument("--md-path-for-index")
    ap.add_argument("--sections", action="store_true", help="append a per-section summary using the earlier report's Sections table")
    a = ap.parse_args()

    mix, sr = env.load_audio(a.mix)
    dur = len(mix) / sr
    ws = a.start
    we = dur if a.end == "end" else float(a.end)
    stems = {}
    for k in ("bass", "drums", "other", "vocals"):
        p = os.path.join(a.stems_dir, k + ".wav")
        if os.path.exists(p):
            stems[k], _ = env.load_audio(p)
    notes = {}
    for k in ("mix", "bass", "other", "vocals"):
        p = getattr(a, "csv_" + k)
        notes[k] = sel(env.read_notes(p), ws, we) if p and os.path.exists(p) else np.zeros((0, 4))
    info = parse_report(a.report_md, ws, we)
    bpb = info["bpb"] or a.beats_per_bar
    pulse = info["pulse"]
    reliable = pulse is not None and pulse >= a.pulse_min
    if pulse is None and a.bpm:
        reliable = True
    # bar intervals
    bars = []
    if info["bars"]:
        rows = [r for r in info["bars"] if ws - 0.2 <= r[1] < we]
        diffs = [rows[i + 1][1] - rows[i][1] for i in range(len(rows) - 1)]
        bl0 = float(np.median(diffs)) if diffs else bpb * 60.0 / (info["bpm"] or 120.0)
        diffs = [d for d in diffs if d < 1.5 * bl0]
        bl = float(np.median(diffs)) if diffs else bl0
        # bars outside the earlier report's windows (dropped windows, file start and end) are filled at the bar length
        # of the earlier report's section tempo (else the median bar length); they have no beat-phase alignment
        def blen(t):
            for s0, s1, tp in info["sections"]:
                try:
                    if s0 <= t < s1 and float(tp) > 0:
                        return bpb * 60.0 / float(tp)
                except ValueError:
                    pass
            return bl

        def fill(t, stop):
            while stop - t > 0.6 * blen(t) and t < we:
                bars.append([0, t, t + blen(t), None, False])
                t += blen(t)
            return t

        cur = ws
        for i, r in enumerate(rows):
            cur = fill(cur, r[1])
            nxt = rows[i + 1][1] if i + 1 < len(rows) else we
            end = r[1] + bl if nxt - r[1] > 1.5 * bl else nxt
            bars.append([0, r[1], end, r[2], True])
            cur = end
        fill(cur, we)
        for i, b in enumerate(bars):
            b[0] = i + 1
    elif a.bpm:
        bl = bpb * 60.0 / a.bpm
        t = a.first_beat if a.first_beat is not None else ws
        k = 1
        while t < we:
            bars.append([k, t, t + bl, None, False])
            t += bl
            k += 1
    else:
        sys.exit("need --report-md or --bpm")
    bl = float(np.median([b[2] - b[1] for b in bars]))
    tempo = 60.0 * bpb / bl
    L = []
    w = L.append

    def label(b):
        return ("%d" % b[0]) if reliable else "t=%.2f-%.2f" % (b[1], b[2])

    unit = "beats" if reliable else "seconds"
    w("# %s window %.1f-%.1f s" % (a.name, ws, we))
    w("")
    w("All values are measurements of separated stems and transcribed notes; nothing here is a listening verdict.")
    w("Stems come from demucs htdemucs and notes from basic-pitch, so every ghost note and every missed note of those tools is in here.")
    w("")
    w("## 1. Tempo and grid")
    w("")
    w("- tempo used: %.1f BPM from the earlier report grid (beat period %.3f s)" % (tempo, bl / bpb))
    w("- bar length used: median %.3f s, %d beats per bar, %d bars in the window" % (bl, bpb, len(bars)))
    nfill = sum(1 for b in bars if len(b) > 4 and not b[4])
    if nfill and info["bars"]:
        w("- %d of %d bars lie outside the earlier report's bar tables and were laid out at the earlier report's section tempo, so their phase against the real beat is unknown" % (nfill, len(bars)))
    if pulse is not None:
        w("- pulse strength of the earlier report: %.2f" % pulse)
    if reliable:
        w("- grid status: usable; note lengths are in beats (a quarter beat is a 16th for 4 beats per bar)")
    else:
        w("- grid status: UNRELIABLE (pulse strength under %.2f), so bars are labelled by time in seconds and note lengths are in seconds" % a.pulse_min)
    w("")

    # lines per bar
    def bar_notes(arr, b):
        return [n for n in arr if b[1] <= n[0] < b[2] and n[1] - n[0] >= MIN_NOTE]

    # ---- bass
    w("## 2. Bass line")
    w("")
    w("Notes are name:length, notes under 60 ms dropped. Agreement compares the pitch class of the DSP bass note with the pitch class that has the most transcribed sounding time in the bar (a dash on both sides counts as agreement).")
    w("")
    w("| bar | transcribed bass (name:%s) | DSP bass | agree |" % unit)
    w("|---|---|---|---|")
    agree = []
    for b in bars:
        bn = bar_notes(notes["bass"], b)
        beat = (b[2] - b[1]) / bpb
        txt = " ".join("%s:%s" % (midi_name(n[2]), fmt_len(n[1] - n[0], beat, reliable)) for n in bn) or "-"
        tw = collections.Counter()
        for n in notes["bass"]:
            o = min(n[1], b[2]) - max(n[0], b[1])
            if o > 0 and n[1] - n[0] >= MIN_NOTE:
                tw[int(n[2]) % 12] += o
        mine = max(tw, key=lambda k: tw[k]) if tw else None
        dsp = b[3]
        if info["bars"] and b[4]:
            ok = (mine is None and dsp is None) or (mine is not None and dsp is not None and note_pc(dsp) == mine)
            agree.append(ok)
            ag = "yes" if ok else "no"
        else:
            ag = "n/a"
        w("| %s | %s | %s | %s |" % (label(b), txt, dsp or ("-" if b[4] else "n/a"), ag))
    w("")

    # ---- melody
    vr = None
    if "vocals" in stems and "other" in stems:
        i0, i1 = int(ws * sr), int(we * sr)
        vr = rms_db(stems["vocals"][i0:i1]) - rms_db(stems["other"][i0:i1])
    top_segs = {}
    pats = []
    sources = [("other", notes["other"])]
    if vr is not None and vr >= -12.0:
        sources.append(("vocals", notes["vocals"]))
    w("## 3. Melody / top line")
    w("")
    if vr is not None:
        w("Vocals stem RMS relative to the other stem in this window: %.1f dB, so the vocals stem %s." % (
            vr, "is included (within 12 dB)" if vr >= -12 else "is not included (more than 12 dB below)"))
    else:
        w("Vocals or other stem missing, so only the available stem is used.")
    w("Per bar the highest sounding note line, notes under 60 ms dropped, as name:%s. A second line is shown only when a note at least 3 semitones below the top sounds for more than half of the bar's top-line time." % unit)
    w("")
    for sname, arr in sources:
        w("### %s stem" % sname)
        w("")
        w("| bar | top line | second line |")
        w("|---|---|---|")
        top, sec = lines_of(arr, ws, we)
        st, ss = segments(top, ws), segments(sec, ws)
        top_segs[sname] = st
        for b in bars:
            beat = (b[2] - b[1]) / bpb
            tl = [s for s in st if b[1] <= s[0] < b[2]]
            fa, fb = int((b[1] - ws) / 0.01), int((b[2] - ws) / 0.01)
            tt = (top[fa:fb] >= 0).sum()
            sc = (sec[fa:fb] >= 0).sum()
            sl = [s for s in ss if b[1] <= s[0] < b[2]] if tt and sc / tt > 0.5 else []
            f = lambda l: " ".join("%s:%s" % (midi_name(s[2]), fmt_len(s[1], beat, reliable)) for s in l) or "-"
            w("| %s | %s | %s |" % (label(b), f(tl), f(sl)))
        w("")

    # ---- chords
    w("## 4. Chords")
    w("")
    w("Pitch classes are those sounding at least a quarter of the bar across the bass and other transcriptions. The label is the best of 9 triad and seventh templates; confidence is the share of note energy (duration times velocity) whose pitch class is a chord tone.")
    w("")
    w("| bar | pitch classes | chord | confidence |")
    w("|---|---|---|---|")
    bo = np.vstack([notes["bass"], notes["other"]]) if len(notes["bass"]) + len(notes["other"]) else np.zeros((0, 4))
    for b in bars:
        pcs, lab, cf = chord_of(bo, b[1], b[2])
        w("| %s | %s | %s | %.2f |" % (label(b), " ".join(NAMES[p] for p in pcs) or "-", lab, cf))
    w("")

    # ---- drums
    w("## 5. Drum pattern")
    w("")
    nst = 4 * bpb
    w("Rows are %d steps per bar (x onset, . none), steps are quarter beats of the bar grid; low is onset energy mostly under 150 Hz, mid is 150 Hz to 2 kHz, high is above 5 kHz (a high mark is also set under a low onset when the high share is at least 0.35)." % nst)
    if not reliable:
        w("The grid is unreliable, so the step positions are only as good as the bar boundaries.")
    w("")
    if "drums" in stems:
        ons = drum_onsets(stems["drums"], sr, ws, we)
        w("| bar | low | mid | high |")
        w("|---|---|---|---|")
        for bi, b in enumerate(bars):
            rows = {c: ["."] * nst for c in "LMH"}
            step = (b[2] - b[1]) / nst
            for t, cls in ons:
                if b[1] - step / 2 <= t < b[2] - step / 2:
                    k = min(nst - 1, max(0, int(round((t - b[1]) / step))))
                    for c in cls:
                        rows[c][k] = "x"
            r = ["".join(rows[c]) for c in "LMH"]
            pats.append(tuple(r))
            w("| %s | %s | %s | %s |" % (label(b), r[0], r[1], r[2]))
        w("")
        if pats:
            full = pats
            (pt, cnt), = collections.Counter(full).most_common(1)
            w("Most common bar pattern: low `%s` mid `%s` high `%s`, matched by %d of %d bars." % (pt[0], pt[1], pt[2], cnt, len(full)))
        w("")
        w("Drum onsets found: %d." % len(ons))
    else:
        w("No drums stem available.")
    w("")

    # ---- voice
    w("## 6. Vocals stem level (whole file)")
    w("")
    w("This is only what the separator put in the vocals stem, not a verdict that anything is sung.")
    w("")
    if "vocals" in stems:
        blk = int(2 * sr)
        vals = []
        for i in range(0, len(mix) - blk // 2, blk):
            v = rms_db(stems["vocals"][i:i + blk]) - rms_db(mix[i:i + blk])
            vals.append((i / sr, max(v, -99.0)))
        w("Per 2 s block (start s:dB relative to the mix): " + " ".join("%d:%.0f" % (t, v) for t, v in vals))
        w("")
        rng, cur = [], None
        for t, v in vals:
            if v >= -15.0:
                if cur and abs(cur[1] - t) < 1e-6:
                    cur[1] = t + 2
                else:
                    cur = [t, t + 2]
                    rng.append(cur)
        w("Time ranges within 15 dB of the mix: " + (", ".join("%.0f-%.0f s" % (r[0], min(r[1], dur)) for r in rng) or "none") + ".")
    else:
        w("No vocals stem available.")
    w("")

    # ---- envelopes
    w("## 7. Envelopes")
    w("")
    w("Per note on the stem, band-passed around the note pitch: attack is 10 percent to 90 percent of the peak amplitude, peak to sustain is the time until the level is within 1 dB of the sustain, sustain is the median of the middle half of the note in dB relative to the peak, release is from the transcribed note end to 20 dB below the sustain, centroid is the spectral centroid of the raw stem over the first 50 ms (attack) and over the middle half (sustain) with their ratio. Medians with the 10th to 90th percentile; fewer than 5 notes is not measurable.")
    w("The other stem is measured only on notes inside stretches of at least 2 s with at most one transcribed note sounding.")
    w("")
    for sname, iso in (("bass", False), ("other", True)):
        if sname in stems and len(notes[sname]):
            rows = env.measure_notes(stems[sname], sr, env.read_notes(getattr(a, "csv_" + sname)), ws, we, iso)
            res = env.summarize(rows)
            w("- %s%s: %d notes measured. %s" % (sname, " (isolated stretches)" if iso else "", len(rows), env.fmt_summary(res)))
        else:
            w("- %s: not measurable (no notes or no stem)" % sname)
    w("")

    # ---- confidence
    w("## 8. Confidence numbers")
    w("")
    shorts = {}
    for k in ("mix", "bass", "other", "vocals"):
        n = notes[k]
        shorts[k] = (float(np.mean((n[:, 1] - n[:, 0]) < MIN_NOTE)) if len(n) else float("nan"))
    stem_all = np.vstack([notes[k] for k in ("bass", "other", "vocals")])
    hit = 0
    for n in notes["mix"]:
        if len(stem_all) and np.any((stem_all[:, 2] == n[2]) & (np.abs(stem_all[:, 0] - n[0]) <= 0.08)):
            hit += 1
    mixin = hit / len(notes["mix"]) if len(notes["mix"]) else float("nan")
    keys = keys_from_prompt(a.prompt)
    kc = key_corr(notes["mix"], keys) if keys else None
    bag = float(np.mean(agree)) if agree else float("nan")
    w("- share of notes shorter than 60 ms: mix %.3f, bass %.3f, other %.3f, vocals %.3f" % (
        shorts["mix"], shorts["bass"], shorts["other"], shorts["vocals"]))
    w("- share of mix notes also found in a stem transcription (same pitch, onset within 80 ms): %.3f" % mixin)
    if kc:
        w("- pitch-class histogram correlation with the prompt key (%s; best of %s): %.3f" % (
            kc[1], ", ".join("%s %s" % k for k in keys), kc[0]))
    else:
        w("- pitch-class histogram correlation with the prompt key: no key named in the prompt")
    w("- share of bars where the transcribed bass and the earlier DSP bass agree: %s" % ("%.3f" % bag if agree else "n/a"))
    w("- notes counted in the window: mix %d, bass %d, other %d, vocals %d" % tuple(len(notes[k]) for k in ("mix", "bass", "other", "vocals")))
    w("")
    if a.sections and info["sections"]:
        w("## 9. Per-section summary")
        w("")
        w("Sections are the boundaries of the earlier report; tempo is the earlier report's per-section tempo.")
        w("Bass cycle is the transcribed bass pitch with the most sounding time per bar, written note x bars. Drums show the most common bar pattern of the section. Lead counts transcribed notes at or above C5 (MIDI 72) with onset in each bar, and the contour is first, highest and last top-line note per bar. Density is transcribed notes per second.")
        w("")
        for si, (s0, s1, tp) in enumerate(info["sections"]):
            ib = [i for i, b in enumerate(bars) if s0 - 0.01 <= b[1] < s1]
            w("### Section %d: %.1f-%.1f s, tempo %s BPM, %d bars" % (si + 1, s0, s1, tp, len(ib)))
            w("")
            runs = []
            for i in ib:
                b = bars[i]
                tw = collections.Counter()
                for n in notes["bass"]:
                    o = min(n[1], b[2]) - max(n[0], b[1])
                    if o > 0 and n[1] - n[0] >= MIN_NOTE:
                        tw[int(n[2])] += o
                nm = midi_name(max(tw, key=lambda k: tw[k])) if tw else "-"
                if runs and runs[-1][0] == nm:
                    runs[-1][1] += 1
                else:
                    runs.append([nm, 1])
            w("- bass cycle: " + (" ".join("%sx%d" % (r[0], r[1]) for r in runs[:40]) or "none") + (" ..." if len(runs) > 40 else ""))
            if pats and ib:
                (pt, cnt), = collections.Counter(pats[i] for i in ib if i < len(pats)).most_common(1)
                w("- drums, most common bar (%d of %d bars): low `%s` mid `%s` high `%s`" % (cnt, len(ib), pt[0], pt[1], pt[2]))
            for sname, arr in sources:
                cnts, cont = [], []
                for i in ib:
                    b = bars[i]
                    cnts.append(sum(1 for n in arr if b[1] <= n[0] < b[2] and n[2] >= 72 and n[1] - n[0] >= MIN_NOTE))
                    segs = [x for x in top_segs.get(sname, []) if b[1] <= x[0] < b[2]]
                    cont.append("-" if not segs else "%s>%s>%s" % (midi_name(segs[0][2]), midi_name(max(x[2] for x in segs)), midi_name(segs[-1][2])))
                w("- lead (%s stem), notes at or above C5 per bar: %s" % (sname, " ".join(map(str, cnts)) or "none"))
                w("- lead (%s stem), top-line contour per bar (first>highest>last): %s" % (sname, " ".join(cont) or "none"))
            dens = []
            for k in ("mix", "bass", "other", "vocals"):
                n = notes[k]
                c = int(np.sum((n[:, 0] >= s0) & (n[:, 0] < s1))) if len(n) else 0
                dens.append("%s %.2f" % (k, c / max(s1 - s0, 1e-9)))
            w("- note density per second: " + ", ".join(dens))
            w("")
    open(a.out_md, "w").write("\n".join(L) + "\n")

    with open(a.out_tsv, "w") as f:
        f.write("stem\tonset_s\tduration_s\tmidi\tname\tvelocity\n")
        for k in ("mix", "bass", "other", "vocals"):
            for n in notes[k]:
                f.write("%s\t%.3f\t%.3f\t%d\t%s\t%d\n" % (k, n[0], n[1] - n[0], n[2], midi_name(n[2]), n[3]))

    if a.index:
        wl = "%g-%s" % (ws, "end" if a.end == "end" else "%g" % we)
        hdr = ["file", "window", "short_note_share_mix/bass/other/vocals", "mix_notes_in_stems", "key_corr(key)",
               "bass_agree_share", "notes_mix/bass/other/vocals", "md"]
        row = [a.name, wl, "%.3f/%.3f/%.3f/%.3f" % (shorts["mix"], shorts["bass"], shorts["other"], shorts["vocals"]),
               "%.3f" % mixin, ("%.3f(%s)" % (kc[0], kc[1])) if kc else "n/a", "%.3f" % bag if agree else "n/a",
               "/".join(str(len(notes[k])) for k in ("mix", "bass", "other", "vocals")), a.md_path_for_index or a.out_md]
        old = []
        if os.path.exists(a.index):
            old = [l.rstrip("\n") for l in open(a.index) if l.strip() and not l.startswith("file\t")]
        old = [l for l in old if not (l.split("\t")[0] == row[0] and l.split("\t")[1] == row[1])]
        open(a.index, "w").write("\t".join(hdr) + "\n" + "".join(l + "\n" for l in old) + "\t".join(row) + "\n")


if __name__ == "__main__":
    main()
