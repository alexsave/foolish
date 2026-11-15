#!/usr/bin/env python3
"""lyria_notes_extract.py - measure an AI-generated "eight single held notes" clip and cut the clean notes into a sampler instrument.

Usage
  python3 -I lyria_notes_extract.py CLIP --name INSTRUMENT [--measure-only] --out-samples DIR --manifest PATH
                                    --log-rows PATH --report PATH [--asked "E4 F#4 ..."] [--min-len S]
  CLIP is any file ffmpeg can decode (mp3). It is decoded to a temporary wav, the native sample rate and both channels are kept.

What it does
  1. Segments the clip into notes by level. The level is the 10 ms RMS. The noise floor is the 10th percentile of that level.
     A note starts where the level reaches max(floor + 30 dB, clip peak - 40 dB) and ends where it falls below that and stays
     below for 150 ms. The reported end is the first frame below it.
  2. Measures every segment: f0 (YIN from vocalset_extract, median over the 20 to 80 percent stretch), nearest MIDI note and
     cents offset, f0 sd in cents, vibrato rate and depth, attack time (to within 3 dB of the sustain level), decay slope in
     dB/s over the stretch, tail level 300 ms after the end relative to the sustain, PURITY, the three strongest spectral peaks
     that are not near a harmonic of f0, the number of distinct pitches implied, and a second-onset check.
  3. PURITY is the share of spectral energy between 80 Hz and 8 kHz (8192-point Hann windows, hop 2048, over the stretch,
     median over the windows) that sits in spectral peaks lying within 35 cents of an integer multiple of f0. A peak owns the
     bins within 3 bins of its maximum (the Hann main lobe), so the share does not depend on the bin spacing.
     Peaks are local maxima over 9 bins, within 60 dB of the window's strongest. Per window f0 is the median YIN f0 of the
     frames inside it.
     DISTINCT PITCHES is 1 plus one if the fundamental (the k = 1 peak) is absent or more than 25 dB under the strongest
     partial (then f0 is a common subharmonic, as for a fifth) plus the number of groups of non-harmonic peaks above -25 dB re the strongest harmonic partial
     (a peak joins a group if it is within 35 cents of an integer multiple of the group's lowest peak). A peak must be seen in
     at least half of the windows, with its median level over those windows.
     SECOND ONSET: a spectral-flux spike (1024-point Hann, hop 10 ms, positive linear magnitude change, 80 Hz to 8 kHz) more
     than 80 ms after the start that is at least 0.3 of the segment's attack flux (the largest flux from 30 ms before to 200 ms
     after the start) and at least 4 times the median flux within 100 ms around it (a spike, not the ramp of a slow swell).
  4. A segment is accepted only if: one pitch, purity >= 0.90, no second onset, length >= 0.6 s (0.3 s for spanish_guitar),
     f0 sd <= 35 cents. Every rule that fails is listed as the reason.
  5. Each accepted note is cut from 8 ms before the start to the end of its release (the level back to floor + 6 dB, at most
     1.5 s after the end, never into the next note), 3 ms fade in and 30 ms fade out, pitch-corrected to the nearest
     equal-tempered semitone by one constant resampling ratio from the median offset (the same method as vocalset_extract),
     one gain per instrument so the loudest sample peaks at -1 dBFS and relative note levels stay.
     Wavs are 24-bit stereo at the clip's own rate: DIR/<name>_<Note>_<midi>.wav (a second sample of the same root gets _rr1).
  6. The manifest has one line per sample, "root_midi 0 rr path", rr counting from 0, the path written
     /SAMPLES/soundtrack/samples/lyria/<name>/<file> so SAMPLES=<dir> re-roots it (the sampler's rule).
  7. --log-rows appends one TSV row per accepted sample. --report writes the Markdown table of all segments.
  --measure-only does 1, 2, 4 (verdicts) and the report and writes no samples, no manifest, no log rows.

Failure modes
  Undecodable clip, or ffmpeg missing: prints the error and exits 2.
  A clip with no segment: the report says so and nothing is written.
  A wrong f0 octave from YIN is not corrected; the report puts asked and played notes side by side so it shows.
  Deterministic: no randomness, same clip gives byte-identical output.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from fractions import Fraction

import numpy as np
from scipy.io import wavfile
from scipy.ndimage import maximum_filter1d
from scipy.signal import resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vocalset_extract as ve  # noqa: E402  (note_name, midi_to_hz, vibrato, track)

# ---- constants (every threshold has one name)
HOP_S = 0.010
FLOOR_PCT = 10.0
START_ABOVE_FLOOR_DB = 30.0
START_BELOW_PEAK_DB = 40.0
END_HOLD_S = 0.150
STRETCH = (0.2, 0.8)
F0_MIN, F0_MAX = 55.0, 1400.0
NFFT = 8192
WIN_HOP = 2048
BAND = (80.0, 8000.0)
HARM_CENTS = 35.0
PEAK_NEIGH = 9
PEAK_FLOOR_DB = -60.0
LOBE_BINS = 3
PITCH_DB = -25.0
PEAK_PRESENCE = 0.5
ONSET_AFTER_S = 0.080
ONSET_REF_S = (0.030, 0.200)
ONSET_RATIO = 0.3
ONSET_SPIKE = 4.0
ONSET_MEDIAN_S = 0.100
FLUX_WIN = 1024
MIN_PURITY = 0.90
MAX_SD_CENTS = 35.0
MIN_LEN_S = 0.6
MIN_LEN_BY_NAME = {"spanish_guitar": 0.3}
PRE_ROLL_S = 0.008
FADE_IN_S = 0.003
FADE_OUT_S = 0.030
RELEASE_FLOOR_DB = 6.0
MAX_RELEASE_S = 1.5
NEXT_GUARD_S = 0.020
PEAK_TARGET_DB = -1.0
TAIL_AT_S = 0.300
TAIL_LEN_S = 0.050
WHAT = "Lyria generated solo %s single note, cut and pitch-corrected"


def note_name(m):
    return ve.note_name(m)


def parse_note(s):
    """'F#4' or 'Bb3' or 'B-flat3' or 'G-sharp2' to MIDI (C4 = 60)."""
    s = s.replace("-flat", "b").replace("-sharp", "#")
    base = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}[s[0]]
    i = 1
    while i < len(s) and s[i] in "#b":
        base += 1 if s[i] == "#" else -1
        i += 1
    return base + 12 * (int(s[i:]) + 1)


# ---------------------------------------------------------------- io
def decode(path):
    """ffmpeg to a temporary float wav. Returns (float64 array [n, ch], sample rate)."""
    with tempfile.TemporaryDirectory() as d:
        tmp = os.path.join(d, "clip.wav")
        try:
            r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", path, "-f", "wav", "-c:a", "pcm_f32le", tmp],
                               capture_output=True, text=True)
        except OSError as e:
            print("ffmpeg: %s" % e, file=sys.stderr)
            sys.exit(2)
        if r.returncode != 0:
            print("cannot decode %s: %s" % (path, r.stderr.strip()), file=sys.stderr)
            sys.exit(2)
        sr, x = wavfile.read(tmp)
    x = x.astype(np.float64)
    if x.ndim == 1:
        x = x[:, None]
    return x, sr


def write_wav24(path, x, sr):
    """x: [n, ch] float in [-1, 1]. 24-bit PCM."""
    n, ch = x.shape
    v = np.clip(np.round(x * (2 ** 23 - 1)), -(2 ** 23), 2 ** 23 - 1).astype(np.int64)
    v = np.where(v < 0, v + (1 << 24), v).astype(np.uint32).reshape(-1)
    b = np.stack([v & 255, (v >> 8) & 255, (v >> 16) & 255], axis=1).astype(np.uint8).tobytes()
    import struct
    hdr = b"RIFF" + struct.pack("<I", 36 + len(b)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, ch, sr, sr * 3 * ch, 3 * ch, 24)
    hdr += b"data" + struct.pack("<I", len(b))
    with open(path, "wb") as f:
        f.write(hdr + b)


# ---------------------------------------------------------------- measurement
def level_db(m, sr):
    """10 ms RMS in dBFS per frame (non overlapping). Returns (db array, hop samples)."""
    h = int(round(HOP_S * sr))
    nf = len(m) // h
    fr = m[:nf * h].reshape(nf, h)
    return 10.0 * np.log10(np.maximum(np.mean(fr ** 2, axis=1), 1e-14)), h


def segment(m, sr):
    """Returns (list of (start frame, end frame), floor dB, threshold dB, level array, hop)."""
    db, h = level_db(m, sr)
    floor = float(np.percentile(db, FLOOR_PCT))
    thr = max(floor + START_ABOVE_FLOOR_DB, float(db.max()) - START_BELOW_PEAK_DB)
    hold = int(round(END_HOLD_S / HOP_S))
    segs = []
    i, n = 0, len(db)
    while i < n:
        if db[i] >= thr:
            s = i
            while i < n:
                if db[i] < thr and np.all(db[i:i + hold] < thr):
                    break
                i += 1
            segs.append((s, i))
        i += 1
    return segs, floor, thr, db, h


def yin_f0(x, sr):
    """YIN f0 per 10 ms frame (0 = unvoiced) over 55 to 1400 Hz, using vocalset_extract.track."""
    old = (ve.FMIN, ve.FMAX)
    ve.FMIN, ve.FMAX = F0_MIN, F0_MAX
    try:
        f0, _, centers = ve.track(x, sr)
    finally:
        ve.FMIN, ve.FMAX = old
    return f0, centers


def window_spectrum(x, f0):
    """One window: returns (purity, peaks). peaks: list of dict(freq, db, harm, k)."""
    n = len(x)
    w = np.hanning(n)
    z = np.zeros(NFFT)
    z[:n] = x * w
    P = np.abs(np.fft.rfft(z)) ** 2
    return P


def analyse_window(P, sr, f0):
    freqs = np.arange(len(P)) * sr / NFFT
    band = (freqs >= BAND[0]) & (freqs <= BAND[1])
    mx = maximum_filter1d(P, size=PEAK_NEIGH)
    cand = np.nonzero((P == mx) & (P > P.max() * 10.0 ** (PEAK_FLOOR_DB / 10.0)) & (freqs >= 40.0) & (freqs <= 8500.0))[0]
    cand = cand[(cand > 0) & (cand < len(P) - 1)]
    peaks = []
    lp = np.log(np.maximum(P, 1e-30))
    for k in cand:
        a, b, c = lp[k - 1], lp[k], lp[k + 1]
        den = a - 2 * b + c
        d = 0.5 * (a - c) / den if abs(den) > 1e-12 else 0.0
        f = (k + d) * sr / NFFT
        kk = int(round(f / f0))
        harm = kk >= 1 and abs(1200.0 * np.log2(f / (kk * f0))) <= HARM_CENTS
        peaks.append(dict(bin=int(k), freq=f, db=10.0 * np.log10(P[k]), harm=bool(harm), k=kk))
    total = P[band].sum()
    if not peaks or total <= 0:
        return 0.0, peaks
    pb = np.array([p["bin"] for p in peaks])
    bins = np.nonzero(band)[0]
    j = np.clip(np.searchsorted(pb, bins), 1, len(pb) - 1) if len(pb) > 1 else np.zeros(len(bins), dtype=int)
    if len(pb) > 1:
        left, right = pb[j - 1], pb[j]
        j = np.where(np.abs(bins - left) <= np.abs(bins - right), j - 1, j)
    own = np.abs(bins - pb[j]) <= LOBE_BINS
    isharm = np.array([p["harm"] for p in peaks])[j]
    return float(P[bins][own & isharm].sum() / total), peaks


def spectral_flux(m, sr):
    """Positive linear magnitude change per 10 ms frame, 80 Hz to 8 kHz. Returns (flux, frame times s)."""
    h = int(round(HOP_S * sr))
    nf = (len(m) - FLUX_WIN) // h + 1
    if nf < 2:
        return np.zeros(0), np.zeros(0)
    idx = np.arange(nf)[:, None] * h + np.arange(FLUX_WIN)[None, :]
    M = np.abs(np.fft.rfft(m[idx] * np.hanning(FLUX_WIN), axis=1))
    fr = np.arange(M.shape[1]) * sr / FLUX_WIN
    M = M[:, (fr >= BAND[0]) & (fr <= BAND[1])]
    flux = np.zeros(nf)
    flux[1:] = np.maximum(M[1:] - M[:-1], 0.0).sum(axis=1)
    return flux, (np.arange(nf) * h + FLUX_WIN / 2.0) / sr


def second_onset(flux, ft, start, end):
    """Returns the largest spike ratio (flux / attack flux) after start + 80 ms, 0 if none qualifies, and its time."""
    if len(flux) == 0:
        return 0.0, 0.0
    ref_sel = (ft >= start - ONSET_REF_S[0]) & (ft <= start + ONSET_REF_S[1])
    ref = flux[ref_sel].max() if ref_sel.any() else 0.0
    if ref <= 0:
        return 0.0, 0.0
    best, bt = 0.0, 0.0
    hf = int(round(ONSET_MEDIAN_S / HOP_S))
    for i in np.nonzero((ft > start + ONSET_AFTER_S) & (ft <= end))[0]:
        lo, hi = max(0, i - 3), min(len(flux), i + 4)
        if flux[i] < flux[lo:hi].max():
            continue
        med = np.median(flux[max(0, i - hf):i + hf + 1])
        r = flux[i] / ref
        if r >= ONSET_RATIO and flux[i] >= ONSET_SPIKE * max(med, 1e-12) and r > best:
            best, bt = r, float(ft[i])
    return float(best), bt


def measure(m, sr, s, e, nxt, db, h, floor, flux, ft, min_len, min_purity=MIN_PURITY):
    """Measures one segment given start/end seconds. Returns the measurement dict (with 'reasons')."""
    L = e - s
    r = dict(start=s, end=e, length=L)
    a0, a1 = s + STRETCH[0] * L, s + STRETCH[1] * L
    fi0, fi1 = int(a0 / HOP_S), max(int(a0 / HOP_S) + 1, int(a1 / HOP_S))
    st = db[fi0:fi1]
    sus = float(np.median(st))
    r["sus_db"] = sus
    sx = m[int(a0 * sr):int(a1 * sr)]
    f0f, cen = yin_f0(sx, sr)
    voiced = f0f > 0
    r["voiced"] = int(voiced.sum())
    r["frames"] = len(f0f)
    # attack: first frame within 3 dB of the sustain level
    i0 = int(s / HOP_S)
    k = 0
    while i0 + k < len(db) and db[i0 + k] < sus - 3.0:
        k += 1
    r["attack"] = k * HOP_S
    t = np.arange(len(st)) * HOP_S
    r["decay"] = float(np.polyfit(t, st, 1)[0]) if len(st) >= 3 else 0.0
    tail_a = int((e + TAIL_AT_S) * sr)
    tail_b = int((e + TAIL_AT_S + TAIL_LEN_S) * sr)
    if tail_b <= len(m) and (nxt is None or e + TAIL_AT_S + TAIL_LEN_S < nxt):
        r["tail_db"] = 10.0 * np.log10(max(np.mean(m[tail_a:tail_b] ** 2), 1e-14)) - sus
    else:
        r["tail_db"] = None
    r["onset2"], r["onset2_t"] = second_onset(flux, ft, s, e)
    r["extra"] = []
    r["purity"] = 0.0
    r["pitches"] = 0
    r["fund_rel"] = -99.0
    if voiced.sum() < 3:
        r.update(f0=0.0, midi=0, cents=0.0, sd=0.0, vib_rate=0.0, vib_depth=0.0)
        r["reasons"] = ["no_f0"]
        if r["length"] < min_len:
            r["reasons"].append("length")
        return r
    fv = f0f[voiced]
    med = float(np.median(fv))
    cents = 1200.0 * np.log2(fv / med)
    r["f0"] = med
    r["midi"] = int(round(69 + 12 * np.log2(med / 440.0)))
    r["cents"] = float(1200.0 * np.log2(med / ve.midi_to_hz(r["midi"])))
    r["sd"] = float(np.std(cents))
    r["vib_rate"], r["vib_depth"] = ve.vibrato(1200.0 * np.log2(np.where(voiced, f0f, med) / med), HOP_S)
    # spectral purity over windows of the stretch
    wins = []
    if len(sx) <= NFFT:
        wins.append((0, len(sx)))
    else:
        for a in range(0, len(sx) - NFFT + 1, WIN_HOP):
            wins.append((a, a + NFFT))
    purs, perwin = [], []
    for (a, b) in wins:
        inside = (cen >= a) & (cen < b) & voiced
        f0w = float(np.median(f0f[inside])) if inside.any() else med
        P = window_spectrum(sx[a:b], f0w)
        pu, peaks = analyse_window(P, sr, f0w)
        purs.append(pu)
        perwin.append((f0w, peaks))
    r["purity"] = float(np.median(purs))
    r["extra"], r["pitches"], r["fund_rel"] = nonharmonic(perwin, sr)
    # verdict
    why = []
    if r["pitches"] != 1:
        why.append("pitch")
    if r["purity"] < min_purity:
        why.append("purity")
    if r["onset2"] > 0:
        why.append("second_onset")
    if r["length"] < min_len:
        why.append("length")
    if r["sd"] > MAX_SD_CENTS:
        why.append("stability")
    r["reasons"] = why
    return r


def nonharmonic(perwin, sr):
    """Clusters the non-harmonic peaks of all windows. Returns (top 3 clusters, number of distinct pitches, fundamental level)."""
    clusters = []   # dict(freq list, rel_fund list, rel_strong list)
    fund_rel = []   # per window: fundamental level re the strongest harmonic partial (-99 if there is no k = 1 peak)
    for wi, (f0w, peaks) in enumerate(perwin):
        harm = [p for p in peaks if p["harm"]]
        if not harm:
            continue
        strong = max(p["db"] for p in harm)
        fund = [p["db"] for p in harm if p["k"] == 1]
        fund_db = max(fund) if fund else strong
        fund_rel.append(fund_db - strong if fund else -99.0)
        for p in peaks:
            if p["harm"]:
                continue
            for c in clusters:
                if abs(1200.0 * np.log2(p["freq"] / np.median(c["f"]))) <= HARM_CENTS:
                    break
            else:
                c = dict(f=[], rf=[], rs=[], w=set())
                clusters.append(c)
            c["f"].append(p["freq"])
            c["w"].add(wi)
            c["rf"].append(p["db"] - fund_db)
            c["rs"].append(p["db"] - strong)
    nw = len(perwin)
    out = []
    for c in clusters:
        if len(c["w"]) < max(1.0, PEAK_PRESENCE * nw):
            continue
        f = float(np.median(c["f"]))
        mid = int(round(69 + 12 * np.log2(f / 440.0)))
        out.append(dict(freq=f, note=note_name(mid), rel_fund=float(np.median(c["rf"])),
                        rel_strong=float(np.median(c["rs"])), seen=len(c["w"]), windows=nw))
    out.sort(key=lambda c: -c["rel_strong"])
    loud = sorted([c for c in out if c["rel_strong"] > PITCH_DB], key=lambda c: c["freq"])
    groups = []
    for c in loud:
        for g in groups:
            kk = int(round(c["freq"] / g))
            if kk >= 1 and abs(1200.0 * np.log2(c["freq"] / (kk * g))) <= HARM_CENTS:
                break
        else:
            groups.append(c["freq"])
    # A missing or weak fundamental means the f0 is the common subharmonic of two notes (a fifth is a note an octave
    # below with its odd partials missing), so it implies one more pitch.
    fr = float(np.median(fund_rel)) if fund_rel else -99.0
    return out[:3], 1 + len(groups) + (1 if fr < PITCH_DB else 0), fr


def analyse(x, sr, min_len, min_purity=MIN_PURITY):
    """x [n, ch]. Returns (list of segment dicts, floor dB, threshold dB)."""
    m = x.mean(axis=1)
    segs, floor, thr, db, h = segment(m, sr)
    flux, ft = spectral_flux(m, sr)
    out = []
    for j, (a, b) in enumerate(segs):
        s, e = a * HOP_S, b * HOP_S
        nxt = segs[j + 1][0] * HOP_S if j + 1 < len(segs) else None
        r = measure(m, sr, s, e, nxt, db, h, floor, flux, ft, min_len, min_purity)
        r["next_start"] = nxt
        r["prev_end"] = segs[j - 1][1] * HOP_S if j > 0 else 0.0
        out.append(r)
    return out, floor, thr, db


def pitch_runs(m, sr, db):
    """Stretches of at least 150 ms where the YIN f0 stays within 50 cents of its running median (3 unvoiced frames allowed).
    Investigation only: shows what a clip plays when it has no silences for the level segmenter to find.
    Returns (start s, end s, midi, cents, level dB) per run."""
    f0, cen = yin_f0(m, sr)
    t = cen / sr
    runs, cur = [], []

    def close():
        if len(cur) >= 15:
            med = float(np.median(f0[cur]))
            mid = int(round(69 + 12 * np.log2(med / 440.0)))
            a, b = int(t[cur[0]] / HOP_S), int(t[cur[-1]] / HOP_S) + 1
            runs.append((float(t[cur[0]]), float(t[cur[-1]]), mid, float(1200.0 * np.log2(med / ve.midi_to_hz(mid))),
                         float(np.median(db[a:b]))))
    for i in range(len(f0)):
        if f0[i] > 0 and (not cur or abs(1200.0 * np.log2(f0[i] / np.median(f0[cur[-10:]]))) < 50.0):
            cur.append(i)
        elif f0[i] > 0:
            close()
            cur = [i]
        elif cur and i - cur[-1] > 3:
            close()
            cur = []
    close()
    return runs


# ---------------------------------------------------------------- cutting
def cut(x, sr, r, db, floor):
    """Returns (pitch-corrected stereo audio, info). Cut window, fades, one resampling ratio."""
    s, e = r["start"], r["end"]
    a0 = max(int(round((s - PRE_ROLL_S) * sr)), int(round(r["prev_end"] * sr)) if r["prev_end"] > 0 else 0)
    i = int(e / HOP_S)
    lim = e + MAX_RELEASE_S
    if r["next_start"] is not None:
        lim = min(lim, r["next_start"] - NEXT_GUARD_S)
    while i < len(db) and i * HOP_S < lim and db[i] > floor + RELEASE_FLOOR_DB:
        i += 1
    end_t = min(max(i * HOP_S, e), lim, len(x) / sr)
    a1 = int(round(end_t * sr))
    y = x[a0:a1].copy()
    nfi, nfo = int(FADE_IN_S * sr), int(FADE_OUT_S * sr)
    y[:nfi] *= np.linspace(0.0, 1.0, nfi)[:, None]
    y[len(y) - nfo:] *= np.linspace(1.0, 0.0, nfo)[:, None]
    # f0 lands on equal temperament: the audio is replayed (resampled) by f_target / f_measured
    ratio = ve.midi_to_hz(r["midi"]) / r["f0"]
    fr = Fraction(1.0 / ratio).limit_denominator(600)
    z = np.stack([resample_poly(y[:, c], fr.numerator, fr.denominator) for c in range(y.shape[1])], axis=1)
    return z, dict(a0=a0, a1=a1, ratio=ratio)


def sample_file(name, midi, rr):
    return "%s_%s_%d%s.wav" % (name, note_name(midi), midi, "" if rr == 0 else "_rr%d" % rr)


# ---------------------------------------------------------------- report
def fmt_extra(r):
    if not r["extra"]:
        return "none"
    return "; ".join("%.0f Hz %s %+.0f dB re f0 (seen %d/%d)" % (c["freq"], c["note"], c["rel_fund"], c["seen"], c["windows"])
                     for c in r["extra"])


def write_report(path, clip, name, sr, floor, thr, rows, asked, min_len, written, peak=None, runs=None):
    L = ["# Lyria note clip: %s" % name, ""]
    L.append("Clip: `%s`." % clip)
    L.append("Sample rate %d Hz, noise floor %.1f dBFS (10 ms RMS, 10th percentile), segment threshold %.1f dBFS%s." % (
        sr, floor, thr, "" if peak is None else ", clip peak %.1f dBFS" % peak))
    if not rows:
        L.append("No segment found: the level never stays above the threshold and then below it for 150 ms, so the clip has no silences for the level segmenter to find.")
    L.append("Accept rules: one pitch, purity at least %.2f, no second onset, length at least %.1f s, f0 sd at most %.0f cents." %
             (MIN_PURITY, min_len, MAX_SD_CENTS))
    L.append("Note names use C4 = MIDI 60, A4 = 440 Hz.")
    L.append("")
    played = [note_name(r["midi"]) if r.get("midi") else "?" for r in rows]
    L.append("Asked notes: %s." % (" ".join(asked) if asked else "not given"))
    L.append("Played notes (one per segment, in order): %s." % (" ".join(played) if played else "none"))
    if asked and len(asked) == len(rows):
        diff = ["%s asked, %s played" % (a, p) for a, p in zip(asked, played) if a != p]
        L.append("Segment count matches the asked count, differing notes: %s." % ("; ".join(diff) if diff else "none"))
    elif asked:
        L.append("Segment count %d differs from the asked count %d." % (len(rows), len(asked)))
    L.append("")
    L.append("| # | start s | end s | len s | asked | played | cents | f0 Hz | f0 sd | vibrato | attack s | decay dB/s | tail dB | purity | pitches | f0 level dB | onset2 | verdict |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for i, r in enumerate(rows):
        a = asked[i] if asked and i < len(asked) else "-"
        vib = "%.1f Hz %.0f c" % (r["vib_rate"], r["vib_depth"]) if r.get("vib_rate", 0) > 0 else "none"
        tail = "%.1f" % r["tail_db"] if r["tail_db"] is not None else "-"
        verdict = "accept" if not r["reasons"] else "reject: " + ", ".join(r["reasons"])
        if r.get("midi"):
            L.append("| %d | %.2f | %.2f | %.2f | %s | %s | %+.1f | %.1f | %.1f | %s | %.2f | %+.1f | %s | %.3f | %d | %s | %s | %s |" % (
                i + 1, r["start"], r["end"], r["length"], a, note_name(r["midi"]), r["cents"], r["f0"], r["sd"], vib,
                r["attack"], r["decay"], tail, r["purity"], r["pitches"], "%.0f" % r["fund_rel"] if r["fund_rel"] > -99 else "absent",
                "%.2f at %.2f s" % (r["onset2"], r["onset2_t"]) if r["onset2"] > 0 else "none", verdict))
        else:
            L.append("| %d | %.2f | %.2f | %.2f | %s | none | - | - | - | - | %.2f | %+.1f | %s | - | - | %s | %s |" % (
                i + 1, r["start"], r["end"], r["length"], a, r["attack"], r["decay"], tail,
                "%.2f" % r["onset2"] if r["onset2"] > 0 else "none", verdict))
    L.append("")
    L.append("Strongest non-harmonic peaks per segment (level re the fundamental, seen in how many analysis windows).")
    L.append("")
    for i, r in enumerate(rows):
        L.append("- Segment %d: %s." % (i + 1, fmt_extra(r)))
    if runs is not None:
        L.append("")
        L.append("Stable pitch runs (at least 150 ms within 50 cents of the running median f0), for information only, never used for cutting.")
        L.append("")
        L.append("| start s | end s | note | cents | level dB |")
        L.append("|---|---|---|---|---|")
        for (a, b, mid, c, lv) in runs:
            L.append("| %.2f | %.2f | %s | %+.0f | %.0f |" % (a, b, note_name(mid), c, lv))
    L.append("")
    acc = [r for r in rows if not r["reasons"]]
    L.append("Accepted %d of %d segments." % (len(acc), len(rows)))
    if written:
        L.append("Samples written: %s." % ", ".join(written))
    else:
        L.append("No samples written.")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")


# ---------------------------------------------------------------- main
def run(clip, name, measure_only, out_samples, manifest, log_rows, report, asked, min_len=None):
    if min_len is None:
        min_len = MIN_LEN_BY_NAME.get(name, MIN_LEN_S)
    x, sr = decode(clip)
    rows, floor, thr, db = analyse(x, sr, min_len)
    acc = [r for r in rows if not r["reasons"]]
    written = []
    if not measure_only and acc:
        cuts = []
        for r in acc:
            z, info = cut(x, sr, r, db, floor)
            cuts.append((r, z, info))
        peak = max(np.max(np.abs(z)) for _, z, _ in cuts)
        gain = 10.0 ** (PEAK_TARGET_DB / 20.0) / peak
        os.makedirs(out_samples, exist_ok=True)
        count = {}
        mlines, logl = [], []
        for r, z, info in sorted(cuts, key=lambda c: (c[0]["midi"], c[0]["start"])):
            rr = count.get(r["midi"], 0)
            count[r["midi"]] = rr + 1
            fn = sample_file(name, r["midi"], rr)
            write_wav24(os.path.join(out_samples, fn), z * gain, sr)
            written.append(fn)
            mlines.append("%d 0 %d /SAMPLES/soundtrack/samples/lyria/%s/%s" % (r["midi"], rr, name, fn))
            logl.append("\t".join([os.path.join(out_samples, fn), clip, "%.3f-%.3f" % (r["start"], r["end"]),
                                   "%s %+.1f cents" % (note_name(r["midi"]), r["cents"]), "%.6f" % info["ratio"],
                                   "%.3f" % r["purity"], WHAT % name]))
        os.makedirs(os.path.dirname(os.path.abspath(manifest)), exist_ok=True)
        with open(manifest, "w") as f:
            f.write("\n".join(mlines) + "\n")
        with open(log_rows, "a") as f:
            f.write("\n".join(logl) + "\n")
    os.makedirs(os.path.dirname(os.path.abspath(report)), exist_ok=True)
    write_report(report, clip, name, sr, floor, thr, rows, asked, min_len, written, float(db.max()), pitch_runs(x.mean(axis=1), sr, db))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("clip")
    ap.add_argument("--name", required=True)
    ap.add_argument("--measure-only", action="store_true")
    ap.add_argument("--out-samples", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--log-rows", required=True)
    ap.add_argument("--report", required=True)
    ap.add_argument("--asked", default="")
    ap.add_argument("--min-len", type=float, default=None)
    a = ap.parse_args()
    rows = run(a.clip, a.name, a.measure_only, a.out_samples, a.manifest, a.log_rows, a.report, a.asked.split(), a.min_len)
    for i, r in enumerate(rows):
        print("%d %.2f-%.2f %s purity %.3f %s" % (i + 1, r["start"], r["end"], note_name(r["midi"]) if r.get("midi") else "?",
                                                  r["purity"], "accept" if not r["reasons"] else "reject " + ",".join(r["reasons"])))


if __name__ == "__main__":
    main()
