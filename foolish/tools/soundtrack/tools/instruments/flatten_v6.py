#!/usr/bin/env python3
"""flatten_v6.py - second flattening pass for a stored sample: removes the pitch, level AND per-harmonic level modulation that
tools/samples/flatten_sample.py (v5) leaves, using sub-cent heterodyne analysis (vibmod) instead of a 5 cent YIN track.

Usage: python3 -I flatten_v6.py IN.wav OUT.wav --root MIDI [--keep-attack-ms N]
  IN is the ORIGINAL cut (not a v5 flat one): pass 0 is flatten_sample.flatten (YIN pitch map and level division, v5), then
  pass 1 (pitch): the fundamental's residual pitch c(t) in cents (median of the voting harmonics 1 to 6, vibmod.analyse at 200 Hz) minus
     its median over the 25 to 90 percent stretch is removed by a second time-varying read (step 2^(-c/1200), cubic spline), clipped
     to 40 cents, held at its edge values and faded in over 0.15 s after the attack and out over the last 0.10 s;
  pass 2 (harmonics): for each harmonic k of the root up to 10 kHz (at most 40) the mono magnitude A_k(t) is taken by heterodyne
     (40 Hz low pass); the gain g_k(t) = median(A_k over 25 to 90 percent) / A_k, limited to +-24 dB and applied to that harmonic's
     component (same gain on both channels) as y += (g_k - 1) * component_k. Harmonics more than 45 dB under the strongest
     are left alone (they are noise-dominated). The gain has the same fades as pass 1. The same pass also removes each harmonic's
     phase deviation from its median over the sustain (phase lock, LOCK_PHASE), so that the sustain is exactly periodic.
  Everything not harmonic (noise, bow, breath) is untouched.
Output: 24 bit wav like flatten_sample. Prints one line: before and after (pitch pp c, level pp dB, harmonic pp dB) over 25 to 90 percent.
A sample whose v5 flattening was UNRELIABLE is copied through unchanged and the line starts with UNRELIABLE.
Deterministic, no randomness."""
import argparse, os, shutil, sys
import numpy as np
from scipy.interpolate import CubicSpline
from scipy.signal import butter, sosfiltfilt
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "samples"))
import flatten_sample as fs
import lyria_notes_extract as le
import vibmod

MAX_C = 40.0
MAX_G_DB = 24.0
FADE_IN_S = 0.15
FADE_OUT_S = 0.10
SUS = (0.25, 0.90)
KMAX = 40
PITCH_ITERS = 3
LOCK_PHASE = True
REL_FLOOR_DB = -45.0


def weight(n, sr, t_start):
    t = np.arange(n) / sr
    w = np.clip((t - t_start) / FADE_IN_S, 0, 1) * np.clip((n / sr - FADE_OUT_S - t) / FADE_OUT_S + 1, 0, 1)
    return w


def sustain_slice(n):
    return slice(int(SUS[0] * n), int(SUS[1] * n))


def pitch_pass(y, sr, root, t_start):
    n = len(y)
    mono = y.mean(axis=1)
    a = vibmod.analyse(mono, sr, vibmod.hz_of(root), nh=8)
    c = a["pitch_c"]
    tc = np.arange(len(c)) / vibmod.DEC
    k = sustain_slice(len(c))
    c = np.clip(c - np.median(c[k]), -MAX_C, MAX_C)
    # edge samples of the heterodyne filters are not trusted: hold the first and last 30 ms
    e = int(0.03 * vibmod.DEC)
    c[:e] = c[e]
    c[-e:] = c[-e - 1]
    cs = np.interp(np.arange(n) / sr, tc, c) * weight(n, sr, t_start)
    step = 2.0 ** (-cs / 1200.0)
    pos, s = [], 0.0
    while s < n - 1:
        pos.append(s)
        i = int(s)
        s += step[i] + (step[min(i + 1, n - 1)] - step[i]) * (s - i)
    pos = np.array(pos)
    return np.stack([CubicSpline(np.arange(n), y[:, ch])(pos) for ch in range(y.shape[1])], axis=1), a["f0"]


def harmonic_pass(y, sr, f0, t_start):
    n = len(y)
    mono = y.mean(axis=1)
    sos = butter(4, 40.0, fs=sr, output="sos")
    t = np.arange(n) / sr
    ks = [k for k in range(1, KMAX + 1) if k * f0 < min(10000.0, 0.45 * sr)]
    amps, phs = {}, {}
    for k in ks:
        z = mono * np.exp(-2j * np.pi * k * f0 * t)
        z = sosfiltfilt(sos, z.real) + 1j * sosfiltfilt(sos, z.imag)
        amps[k] = np.abs(z)
        phs[k] = np.unwrap(np.angle(z))
    peak = max(np.median(amps[k][sustain_slice(n)]) for k in ks)
    w = weight(n, sr, t_start)
    out = y.copy()
    ew = int(0.03 * sr)
    for k in ks:
        A = amps[k]
        T = np.median(A[sustain_slice(n)])
        if T < peak * 10 ** (REL_FLOOR_DB / 20.0):
            continue
        g_db = np.clip(20 * np.log10(T / np.maximum(A, 1e-12)), -MAX_G_DB, MAX_G_DB)
        g_db[:ew] = g_db[ew]
        g_db[-ew:] = g_db[-ew - 1]
        g = 10 ** (g_db * w / 20.0)
        # phase lock: the harmonic's phase deviation from its own median over the sustain is rotated away, so the harmonic is a steady
        # sinusoid of the mean fundamental; every loop jump then lands on the same phase of every harmonic
        th = (phs[k] - np.median(phs[k][sustain_slice(n)])) * w if LOCK_PHASE else 0.0
        rot = np.exp(-1j * th)
        for ch in range(y.shape[1]):
            z = y[:, ch] * np.exp(-2j * np.pi * k * f0 * t)
            z = sosfiltfilt(sos, z.real) + 1j * sosfiltfilt(sos, z.imag)
            e = np.exp(2j * np.pi * k * f0 * t)
            out[:, ch] += 2 * (z * (g * rot - 1.0) * e).real
    return out


def flatten_v6(x, sr, root, keep_ms=0.0, passes=("pitch", "harm")):
    y, ok, share, jump = fs.flatten(x, sr, root, keep_ms)
    if not ok:
        return x, False
    t_start = max(keep_ms / 1000.0, 0.08)
    f0 = vibmod.hz_of(root)
    if "pitch" in passes:
        for _ in range(PITCH_ITERS):
            y, f0 = pitch_pass(y, sr, root, t_start)
    if "harm" in passes:
        y = harmonic_pass(y, sr, f0, t_start)
    return y, True


def stats(x, sr, root):
    m = x.mean(axis=1)
    r = vibmod.measure_note(m[sustain_slice(len(m))], sr, vibmod.hz_of(root))
    return r["pitch"][4], r["level"][4], max(h[4] for h in r["harm"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp"); ap.add_argument("out")
    ap.add_argument("--root", type=int, required=True)
    ap.add_argument("--keep-attack-ms", type=float, default=0.0)
    a = ap.parse_args()
    x, sr = le.decode(a.inp)
    y, ok = flatten_v6(x, sr, a.root, a.keep_attack_ms)
    if not ok:
        shutil.copyfile(a.inp, a.out)
        print("UNRELIABLE: copied unchanged")
        return
    le.write_wav24(a.out, y, sr)
    b, c = stats(x, sr, a.root), stats(y, sr, a.root)
    print("before: pitch %.2f c, level %.2f dB, harm %.2f dB pp | after: %.2f c, %.2f dB, %.2f dB | length %.3f -> %.3f s" % (*b, *c, len(x) / sr, len(y) / sr))


if __name__ == "__main__":
    main()
