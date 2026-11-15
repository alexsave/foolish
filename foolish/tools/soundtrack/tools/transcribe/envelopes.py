#!/usr/bin/env python3
"""envelopes.py - per-note amplitude envelope measurements on one mono stem plus a note CSV.

What it does: for every note of a basic-pitch style CSV (columns start_time_s,end_time_s,pitch_midi,velocity,
pitch_bend; only the first four are read) it band-passes the stem around the note's pitch (0.85 to 2.6 times the
fundamental), takes a RMS envelope (5 ms, or one fundamental period if longer) and measures, per note:
  attack_s        time from 10 percent to 90 percent of the note's peak envelope amplitude
  peak_to_sus_s   time from the peak until the envelope is within 1 dB of the sustain level (0 if it already is)
  sustain_db      sustain level in dB relative to the peak (median of the middle half of the note)
  centroid_attack_hz / centroid_sustain_hz / centroid_ratio: spectral centroid of the raw stem over the first 50 ms
                  and over the middle half of the note, and their ratio (attack over sustain)
  release_s       time from the transcribed note end until the envelope is 20 dB below the sustain level
                  (not measured when the next note starts first, or the level has not fallen 20 dB within
                  min(1.5 s, next note onset))
Input:  STEM (wav, or any file ffmpeg can decode), NOTES (csv), optional --start/--end (onset window, seconds),
        --isolated (only notes that lie inside a stretch of at least 2 s where at most one transcribed note
        sounds at a time), --min-dur (shortest note measured, default 0.12 s).
Output: a TSV of per-note rows on stdout (or --out-tsv) followed by summary lines "# name median p10 p90 n";
        a quantity with fewer than 5 notes prints "not measurable".
Example: tools/transcribe/envelopes.py bass.wav clip_bass_basic_pitch.csv --start 0 --end 30 --isolated
Known failure modes: the transcribed note end is a model output and can be early or late, which moves release_s;
  other notes sounding in the same band leak into the envelope (use --isolated for polyphonic stems);
  piano-like notes that only decay have no sustain, so sustain_db describes the decay middle; notes whose envelope
  already starts above 10 percent of the peak have no attack measure; no randomness is used anywhere.
"""
import argparse
import subprocess
import sys

import numpy as np
from scipy import signal
from scipy.io import wavfile
from scipy.ndimage import uniform_filter1d

NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def midi_name(m):
    m = int(m)
    return "%s%d" % (NAMES[m % 12], m // 12 - 1)


def load_audio(path, sr=44100):
    """Mono float32 signal and its sample rate. Wav is read directly, other formats through ffmpeg at `sr`."""
    if path.lower().endswith(".wav"):
        rate, x = wavfile.read(path)
        if x.dtype == np.int16:
            x = x.astype(np.float32) / 32768.0
        elif x.dtype == np.int32:
            x = x.astype(np.float32) / 2147483648.0
        else:
            x = x.astype(np.float32)
        if x.ndim > 1:
            x = x.mean(axis=1)
        return x, rate
    out = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "f32le", "-ac", "1", "-ar", str(sr), "-"],
                         check=True, stdout=subprocess.PIPE).stdout
    return np.frombuffer(out, dtype=np.float32).copy(), sr


def read_notes(path):
    """Array of rows (onset, offset, midi, velocity); only the first five csv columns are parsed."""
    rows = []
    with open(path) as f:
        next(f)
        for line in f:
            p = line.strip().split(",")
            if len(p) >= 4:
                rows.append((float(p[0]), float(p[1]), float(p[2]), float(p[3])))
    a = np.array(rows, dtype=float).reshape(-1, 4)
    return a[np.argsort(a[:, 0], kind="stable")]


def isolated_mask(notes, min_stretch=2.0):
    """True for notes that lie wholly inside a stretch (at least min_stretch long) with at most one note sounding."""
    if len(notes) == 0:
        return np.zeros(0, bool)
    ev = sorted([(n[0], 1) for n in notes] + [(n[1], -1) for n in notes], key=lambda e: (e[0], e[1]))
    stretches, count, start = [], 0, 0.0
    for t, d in ev:
        new = count + d
        if count <= 1 and new >= 2:
            if t - start >= min_stretch:
                stretches.append((start, t))
        if count >= 2 and new <= 1:
            start = t
        count = new
    end = ev[-1][0]
    if count <= 1 and end - start >= min_stretch:
        stretches.append((start, end))
    mask = np.zeros(len(notes), bool)
    for i, n in enumerate(notes):
        for a, b in stretches:
            if n[0] >= a - 1e-9 and n[1] <= b + 1e-9:
                mask[i] = True
                break
    return mask


def centroid(x, sr, t0, t1):
    """Magnitude-weighted spectral centroid (Hz) of the raw stem between t0 and t1 (Hann window, 20 Hz and up)."""
    seg = x[max(0, int(t0 * sr)):int(t1 * sr)].astype(np.float64)
    if len(seg) < 64:
        return np.nan
    m = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n=max(4096, len(seg))))
    f = np.fft.rfftfreq(max(4096, len(seg)), 1.0 / sr)
    k = f >= 20
    return float((f[k] * m[k]).sum() / (m[k].sum() + 1e-20))


def measure_note(x, sr, on, off, midi, next_on):
    f0 = 440.0 * 2 ** ((midi - 69) / 12.0)
    lo, hi = 0.85 * f0, min(2.6 * f0, 0.45 * sr)
    cap = off + 1.5
    if next_on is not None:
        cap = min(cap, next_on)
    a, b = max(0, int((on - 0.06) * sr)), min(len(x), int((max(cap, off + 0.05) + 0.02) * sr))
    seg = x[a:b].astype(np.float64)
    if len(seg) < int(0.1 * sr):
        return None
    sos = signal.butter(4, [lo / (sr / 2), hi / (sr / 2)], btype="band", output="sos")
    y = signal.sosfiltfilt(sos, seg)
    w = max(1, int(max(0.005, 1.0 / f0) * sr))  # at least one period of the fundamental, or low notes ripple
    env = np.sqrt(np.maximum(uniform_filter1d(y * y, w, mode="nearest"), 0))
    t = (np.arange(len(env)) + a) / sr
    i_on = int(np.searchsorted(t, on - 0.03))
    i_off = int(np.searchsorted(t, off))
    if i_off - i_on < 4:
        return None
    pk = i_on + int(np.argmax(env[i_on:i_off]))
    peak = env[pk]
    if peak <= 1e-6:
        return None
    # attack: 10 percent crossing (last one before the peak) to the first 90 percent crossing after it
    attack = np.nan
    below = np.where(env[:pk] < 0.1 * peak)[0]
    if len(below):
        i10 = below[-1]
        up = np.where(env[i10:] >= 0.9 * peak)[0]
        if len(up):
            attack = t[i10 + up[0]] - t[i10]
    d = off - on
    m0, m1 = int(np.searchsorted(t, on + 0.25 * d)), int(np.searchsorted(t, on + 0.75 * d))
    if m1 <= m0:
        m1 = m0 + 1
    sus = float(np.median(env[m0:m1]))
    sus_db = 20 * np.log10(max(sus, 1e-9) / peak)
    thr = sus * 10 ** (1.0 / 20)
    after = np.where(env[pk:i_off] <= thr)[0]
    p2s = (t[pk + after[0]] - t[pk]) if len(after) else np.nan
    rel = np.nan
    if next_on is None or next_on >= off - 1e-9:
        tail = np.where(env[i_off:] <= 0.1 * sus)[0]
        if len(tail):
            rel = t[i_off + tail[0]] - off
    ca = centroid(x, sr, on, on + 0.05)
    cs = centroid(x, sr, on + 0.25 * d, on + 0.75 * d)
    ratio = ca / cs if ca == ca and cs == cs and cs > 0 else np.nan
    return attack, p2s, sus_db, rel, ca, cs, ratio


def measure_notes(x, sr, notes, start=None, end=None, isolated=False, min_dur=0.12):
    mask = isolated_mask(notes) if isolated else np.ones(len(notes), bool)
    out = []
    for i, n in enumerate(notes):
        if start is not None and n[0] < start:
            continue
        if end is not None and n[0] >= end:
            continue
        if not mask[i] or n[1] - n[0] < min_dur:
            continue
        later = notes[notes[:, 0] > n[0] + 0.03, 0]
        nxt = float(later[0]) if len(later) else None
        r = measure_note(x, sr, n[0], n[1], n[2], nxt)
        if r is not None:
            out.append((n[0], n[1] - n[0], int(n[2])) + r)
    return out


FIELDS = ["attack_s", "peak_to_sus_s", "sustain_db", "release_s", "centroid_attack_hz", "centroid_sustain_hz",
          "centroid_ratio"]


def summarize(rows):
    """Dict field -> (median, p10, p90, n) or None when fewer than 5 finite values."""
    res = {}
    for k, f in enumerate(FIELDS):
        v = np.array([r[3 + k] for r in rows], float)
        v = v[np.isfinite(v)]
        res[f] = None if len(v) < 5 else (float(np.median(v)), float(np.percentile(v, 10)),
                                          float(np.percentile(v, 90)), len(v))
    return res


def fmt_summary(res):
    parts = []
    for f in FIELDS:
        r = res[f]
        parts.append("%s not measurable" % f if r is None else
                     "%s median %.3f (p10 %.3f, p90 %.3f, n=%d)" % (f, r[0], r[1], r[2], r[3]))
    return "; ".join(parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("stem")
    ap.add_argument("notes")
    ap.add_argument("--start", type=float)
    ap.add_argument("--end", type=float)
    ap.add_argument("--isolated", action="store_true")
    ap.add_argument("--min-dur", type=float, default=0.12)
    ap.add_argument("--out-tsv")
    a = ap.parse_args()
    x, sr = load_audio(a.stem)
    rows = measure_notes(x, sr, read_notes(a.notes), a.start, a.end, a.isolated, a.min_dur)
    out = open(a.out_tsv, "w") if a.out_tsv else sys.stdout
    out.write("onset_s\tduration_s\tmidi\tname\t" + "\t".join(FIELDS) + "\n")
    for r in rows:
        out.write("%.3f\t%.3f\t%d\t%s\t" % (r[0], r[1], r[2], midi_name(r[2])) +
                  "\t".join("%.4f" % v for v in r[3:]) + "\n")
    res = summarize(rows)
    for f in FIELDS:
        v = res[f]
        out.write("# %s\t%s\n" % (f, "not measurable" if v is None else "median %.4f\tp10 %.4f\tp90 %.4f\tn %d" % v))
    if a.out_tsv:
        out.close()


if __name__ == "__main__":
    main()
