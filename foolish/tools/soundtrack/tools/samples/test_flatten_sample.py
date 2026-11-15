#!/usr/bin/env python3
"""Test for flatten_sample.py (synthetic, deterministic, no randomness).
Samples (44.1 kHz stereo, identical channels, harmonic tone with 8 partials, root A3 = MIDI 57):
  V: 1.5 s, 5.5 Hz vibrato of 20 cents, a 2 dB peak-to-peak tremolo at 5.5 Hz and a slow 3 dB level drift, 3 ms fade in, 40 ms fade out.
  S: 1.2 s, 30 cents sharp of the root, no vibrato, level constant.
  N: 1.2 s on the root with a natural attack (a 60 ms linear rise).
Asserts: V flattened has pitch sd under 2 cents (an independent harmonic-comb estimator on 100 ms windows), level sd under 0.3 dB
(40 ms RMS), mean pitch within 2 cents of the root, length within 1 percent; S comes out on the root (within 2 cents); N with
--keep-attack-ms 80 keeps its 10 to 90 percent rise time within 5 ms; identical input channels give identical output channels; a
sample with a noisy pitch track is copied unchanged and flagged.
Run: python3 -I test_flatten_sample.py   The module under test can be swapped with FLATTEN_PATH (used for the mutation checks).
Mutation checked: no pitch map -> V pitch sd 12.2 cents; pitch map inverted -> V pitch sd 23.2 cents; no level correction -> V level sd
0.80 dB; correction applied inside the attack -> N rise time 45.6 ms becomes 30.8 ms; per-channel independent maps -> the two channels come
out with different lengths and the stereo stack raises a ValueError.
"""
import importlib.util
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
path = os.environ.get("FLATTEN_PATH", os.path.join(HERE, "flatten_sample.py"))
spec = importlib.util.spec_from_file_location("flatten_sample", path)
fl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fl)

SR = 44100
ROOT = 57
F_ROOT = 440.0 * 2 ** ((ROOT - 69) / 12.0)


def tone(secs, cents=0.0, vib=0.0, trem_db=0.0, drift_db=0.0, rise=0.003):
    n = int(secs * SR)
    t = np.arange(n) / SR
    f = F_ROOT * 2 ** ((cents + vib * np.sin(2 * np.pi * 5.5 * t)) / 1200.0)
    ph = 2 * np.pi * np.cumsum(f) / SR
    y = sum(np.sin(k * ph) / k for k in range(1, 9)) * 0.2
    db = (trem_db / 2.0) * np.sin(2 * np.pi * 5.5 * t + 1.0) + drift_db * t / secs
    y = y * 10 ** (db / 20.0)
    env = np.minimum(1.0, np.minimum(t / rise, (secs - t) / 0.040))
    y = y * env
    return np.stack([y, y], axis=1)


def comb_cents(y, sr, root, lo, hi):
    """Independent f0: harmonic-comb search of 100 ms windows over +-100 cents. Returns the cents track (hop 25 ms) over [lo, hi] s."""
    base = 440.0 * 2 ** ((root - 69) / 12.0)
    out = []
    w = int(0.1 * sr)
    nfft = 1 << 16
    for a in range(int(lo * sr), int(hi * sr) - w, int(0.025 * sr)):
        X = np.abs(np.fft.rfft(y[a:a + w] * np.hanning(w), nfft))
        best, bc = -1, 0
        for c in np.arange(-100, 100.5, 0.5):
            f0 = base * 2 ** (c / 1200.0)
            s = sum(X[int(round(k * f0 * nfft / sr))] / k for k in range(1, 6))
            if s > best:
                best, bc = s, c
        out.append(bc)
    return np.array(out)


def mean_cents(y, sr, root):
    nfft = 1 << 19
    X = np.abs(np.fft.rfft(y * np.hanning(len(y)), nfft))
    base = 440.0 * 2 ** ((root - 69) / 12.0)
    best, bc = -1, 0
    for c in np.arange(-60, 60.05, 0.1):
        f0 = base * 2 ** (c / 1200.0)
        s = sum(X[int(round(k * f0 * nfft / sr))] / k for k in range(1, 6))
        if s > best:
            best, bc = s, c
    return bc


def level_sd(y, sr, lo, hi):
    m = y.mean(axis=1)
    w = int(0.040 * sr)
    v = [10 * np.log10(np.mean(m[a:a + w] ** 2) + 1e-14) for a in range(int(lo * sr), int(hi * sr) - w, int(0.01 * sr))]
    return float(np.std(v))


def rise_ms(y, sr):
    m = np.abs(y.mean(axis=1))
    w = int(0.005 * sr)
    e = np.sqrt(np.convolve(m ** 2, np.ones(w) / w, mode="same"))
    top = np.median(e[int(0.3 * len(e)):int(0.7 * len(e))])
    return (np.argmax(e >= 0.9 * top) - np.argmax(e >= 0.1 * top)) / sr * 1000


def check(c, msg):
    if not c:
        print("FAIL: " + msg)
        sys.exit(1)


def main():
    v = tone(1.5, 0.0, 20.0, 2.0, 3.0)
    y, ok, _, _ = fl.flatten(v, SR, ROOT)
    check(ok, "V flagged unreliable")
    n = len(y) / SR
    lo, hi = 0.25 * n, 0.85 * n
    c = comb_cents(y.mean(axis=1), SR, ROOT, lo, hi)
    check(np.std(c) < 2.0, "flattened V pitch sd %.1f cents, expected under 2 (before: %.1f)" % (np.std(c), np.std(comb_cents(v.mean(axis=1), SR, ROOT, 0.25 * 1.5, 0.85 * 1.5))))
    check(level_sd(y, SR, lo, hi) < 0.3, "flattened V level sd %.2f dB, expected under 0.3 (before %.2f)" % (level_sd(y, SR, lo, hi), level_sd(v, SR, 0.375, 1.275)))
    mc = mean_cents(y.mean(axis=1), SR, ROOT)
    check(abs(mc) <= 2.0, "flattened V mean pitch %.1f cents from the root" % mc)
    check(abs(len(y) / len(v) - 1) <= 0.01, "length changed by %.2f percent" % (100 * (len(y) / len(v) - 1)))
    s = tone(1.2, 30.0)
    ys, ok, _, _ = fl.flatten(s, SR, ROOT)
    ms = mean_cents(ys.mean(axis=1), SR, ROOT)
    check(ok and abs(ms) <= 2.0, "sharp sample comes out %.1f cents from the root (it went in 30 sharp)" % ms)
    nat = tone(1.2, 0.0, 0.0, 0.0, 0.0, rise=0.060)
    yn, ok, _, _ = fl.flatten(nat, SR, ROOT, 80.0)
    r0, r1 = rise_ms(nat, SR), rise_ms(yn, SR)
    check(abs(r1 - r0) <= 5.0, "natural attack rise time %.1f ms became %.1f ms" % (r0, r1))
    check(np.array_equal(y[:, 0], y[:, 1]), "identical input channels gave different output channels (max diff %.2e)" % np.max(np.abs(y[:, 0] - y[:, 1])))
    # a noisy pitch track is not trusted: copied unchanged
    rng = np.random.default_rng(12345)
    nz = np.stack([rng.standard_normal(SR) * 0.1] * 2, axis=1)
    yz, ok, share, jump = fl.flatten(nz, SR, ROOT)
    check(not ok and np.array_equal(yz, nz), "a noise sample was flattened (voiced share %.2f)" % share)
    print("PASS: pitch sd %.2f cents, level sd %.2f dB, mean %.1f cents, sharp %.1f cents, rise %.1f to %.1f ms" % (np.std(c), level_sd(y, SR, lo, hi), mc, ms, r0, r1))


main()
