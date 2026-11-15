#!/usr/bin/env python3
"""flatten_sample.py - removes a stored sample's own vibrato and slow level movement so that a sustain loop does not join two
different vibrato phases (the engine's own vibrato LFO, sp[13] and sp[14], puts a continuous vibrato back).

Usage: python3 -I flatten_sample.py IN.wav OUT.wav --root MIDI [--keep-attack-ms N]

Pitch: the f0 of the mono sum is tracked with the YIN of vocalset_extract (hop 5 ms, 55 to 1400 Hz), unvoiced frames are filled
by interpolation in log frequency, the curve is smoothed with a 25 ms moving average, and both channels are read through the same
time-varying map (cubic spline): the read step at source position s is f_root / f0(s), so the output sits on the equal-tempered
pitch of the root throughout (A4 = 440). The length changes by the ratio of the mean pitch to the root pitch (a sample played
sharp becomes longer), nothing else.
Level: the output's level envelope is the RMS over 40 ms smoothed over 60 ms; the gain is the envelope's median over 20 to 90
percent of the sample divided by it, limited to +-9 dB, the same on both channels. The gain is held at its value at 40 ms for the first
40 ms and at its value 60 ms before the end for the last 60 ms, so the existing fades keep their shape. With --keep-attack-ms N the
correction is zero up to N ms and then blends in (in dB) over 30 ms, so a natural attack keeps its shape.
Reliability: if the voiced share of the 10 to 90 percent stretch is under 0.9, or two neighbouring voiced frames differ by more than
80 cents, the pitch track is not trusted: OUT is a copy of IN and the line starts with UNRELIABLE.
Prints one line: before and after pitch sd (cents), largest pitch excursion (cents), level sd (dB, 10 ms RMS over 20 to 90 percent)
and the largest 3 to 8 Hz modulation peak of the pitch (cents peak-to-peak) and of the level (dB peak-to-peak).
Failure modes: input shorter than about 0.3 s, or silent, gives UNRELIABLE; ffmpeg is used to read the wav (any format).
Deterministic, no randomness."""
import argparse
import shutil
import sys
import os
import numpy as np
from scipy.interpolate import CubicSpline

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vocalset_extract as ve  # noqa: E402
import lyria_notes_extract as le  # noqa: E402

HOP_S = 0.005
F_MIN, F_MAX = 55.0, 1400.0
SMOOTH_PITCH_S = 0.025
ENV_WIN_S = 0.040
ENV_SMOOTH_S = 0.060
MAX_CORR_DB = 9.0
BLEND_S = 0.030
HOLD_START_S = 0.040
HOLD_END_S = 0.060
MIN_VOICED = 0.90
MAX_JUMP_CENTS = 80.0
SUSTAIN = (0.20, 0.90)


def f0_curve(m, sr):
    """Returns (frame times s, f0 Hz with 0 for unvoiced) at a 5 ms hop."""
    old = (ve.FMIN, ve.FMAX, ve.HOP_S)
    ve.FMIN, ve.FMAX, ve.HOP_S = F_MIN, F_MAX, HOP_S
    try:
        f0, _, cen = ve.track(m, sr)
    finally:
        ve.FMIN, ve.FMAX, ve.HOP_S = old
    return cen / sr, f0


def reliable(t, f0, dur):
    sel = (t >= 0.10 * dur) & (t <= 0.90 * dur)
    if sel.sum() < 10:
        return False, 0.0, 0.0
    share = float(np.mean(f0[sel] > 0))
    v = f0[sel]
    pairs = (v[1:] > 0) & (v[:-1] > 0)
    jump = float(np.max(np.abs(1200 * np.log2(v[1:][pairs] / v[:-1][pairs])))) if pairs.any() else 0.0
    return share >= MIN_VOICED and jump <= MAX_JUMP_CENTS, share, jump


def smooth_logf(t, f0, n_total_s):
    ok = f0 > 0
    lf = np.log2(np.where(ok, f0, 1.0))
    lf = np.interp(t, t[ok], lf[ok])
    k = max(1, int(round(SMOOTH_PITCH_S / HOP_S)))
    pad = np.pad(lf, (k, k), mode="edge")
    return np.convolve(pad, np.ones(k) / k, mode="same")[k:-k]


def read_map(x, sr, root, pitch_map=True, per_channel=False):
    """x [n, ch]. Returns the pitch-flattened audio [m, ch]."""
    n = len(x)
    m = x.mean(axis=1)
    t, f0 = f0_curve(m, sr)
    froot = ve.midi_to_hz(root)
    if not pitch_map:
        return x.copy()
    out_ch = []
    for c in range(x.shape[1]):
        lf = smooth_logf(t, f0, n / sr)
        if per_channel and c == 1:
            lf = lf + 0.01   # test mutation hook: an independent map per channel
        fs = 2.0 ** np.interp(np.arange(n) / sr, t, lf)
        pos, s = [], 0.0
        step = froot / fs
        while s < n - 1:
            pos.append(s)
            i = int(s)
            s += step[i] + (step[min(i + 1, n - 1)] - step[i]) * (s - i)
        pos = np.array(pos)
        out_ch.append(CubicSpline(np.arange(n), x[:, c])(pos))
    return np.stack(out_ch, axis=1)


def level_gain(y, sr, keep_attack_ms):
    n = len(y)
    mono = y.mean(axis=1)
    w1 = max(1, int(ENV_WIN_S * sr))
    e = np.sqrt(np.convolve(mono ** 2, np.ones(w1) / w1, mode="same"))
    w2 = max(1, int(ENV_SMOOTH_S * sr))
    e = np.convolve(e, np.ones(w2) / w2, mode="same")
    med = np.median(e[int(SUSTAIN[0] * n):int(SUSTAIN[1] * n)])
    g = np.clip(20 * np.log10(med / np.maximum(e, 1e-9)), -MAX_CORR_DB, MAX_CORR_DB)
    a = int(HOLD_START_S * sr)
    b = n - int(HOLD_END_S * sr)
    g[:a] = g[a]
    g[b:] = g[b]
    if keep_attack_ms > 0:
        tt = np.arange(n) / sr
        g = g * np.clip((tt - keep_attack_ms / 1000.0) / BLEND_S, 0, 1)
    return 10 ** (g / 20.0)


def flatten(x, sr, root, keep_attack_ms=0.0, pitch_map=True, level=True, per_channel=False):
    """Returns (flat audio, reliable flag, voiced share, largest neighbouring-frame jump)."""
    m = x.mean(axis=1)
    t, f0 = f0_curve(m, sr)
    ok, share, jump = reliable(t, f0, len(x) / sr)
    if not ok:
        return x, False, share, jump
    y = read_map(x, sr, root, pitch_map, per_channel)
    if level:
        y = y * level_gain(y, sr, keep_attack_ms)[:, None]
    return y, True, share, jump


def mod_peak(v, fs, lo=3.0, hi=8.0):
    v = v - np.mean(v)
    if len(v) < 8:
        return 0.0
    w = np.hanning(len(v))
    X = np.abs(np.fft.rfft(v * w, 1 << 14)) * 2.0 / w.sum()
    fr = np.fft.rfftfreq(1 << 14, 1.0 / fs)
    sel = (fr >= lo) & (fr <= hi)
    return float(2 * X[sel].max())


def stats(x, sr):
    """pitch sd (cents), largest excursion (cents), level sd (dB), 3-8 Hz pitch peak (c pp), level peak (dB pp) over 20 to 90 percent."""
    m = x.mean(axis=1)
    t, f0 = f0_curve(m, sr)
    n = len(m) / sr
    sel = (t >= SUSTAIN[0] * n) & (t <= SUSTAIN[1] * n) & (f0 > 0)
    c = 1200 * np.log2(f0[sel] / np.median(f0[sel]))
    db, _ = le.level_db(m, sr)
    lv = db[int(SUSTAIN[0] * n / 0.01):int(SUSTAIN[1] * n / 0.01)]
    return float(np.std(c)), float(np.max(np.abs(c))), float(np.std(lv)), mod_peak(c, 1.0 / HOP_S), mod_peak(lv, 100.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--root", type=int, required=True)
    ap.add_argument("--keep-attack-ms", type=float, default=0.0)
    a = ap.parse_args()
    x, sr = le.decode(a.inp)
    y, ok, share, jump = flatten(x, sr, a.root, a.keep_attack_ms)
    if not ok:
        shutil.copyfile(a.inp, a.out)
        print("UNRELIABLE voiced %.2f max jump %.0f cents: copied unchanged" % (share, jump))
        return
    le.write_wav24(a.out, y, sr)
    b, c = stats(x, sr), stats(y, sr)
    print("before: pitch sd %.1f c, max excursion %.1f c, level sd %.2f dB, 3-8 Hz peak %.1f c / %.2f dB | after: %.1f c, %.1f c, %.2f dB, %.1f c / %.2f dB | length %.3f -> %.3f s" % (
        b[0], b[1], b[2], b[3], b[4], c[0], c[1], c[2], c[3], c[4], len(x) / sr, len(y) / sr))


if __name__ == "__main__":
    main()
