#!/usr/bin/env python3
"""check_v7.py V6.wav V7.wav ROOT [KEEP_MS] : the per sample checks of a v7 long sample against its v6 source.
  null    largest absolute difference (in 24 bit steps) between v7 and v6 over the kept attack and between the v7 tail and the v6 tail
          (must be 0 or 1: identical);
  steps   largest 10 ms RMS step (dB) of v7 over the splice zones, and of the whole sample for comparison;
  recur   share of 100 ms windows of the residual (harmonics 1 to 40 removed, as residual_check.py) over 1 to 6 s of v7 whose best
          normalised correlation with the residual 0.1 to 1.0 s earlier is over 0.5 (echo test; fresh noise gives about 0);
  ripple  the envelope repeat ripple (as artifact_check.py: level envelope autocorrelation peak at 40 to 300 ms times its std, dB) over 1 to 6 s;
  hnr     harmonic to noise ratio dB: the steady harmonic model of the file (extend_v7.project) against what is left, over the v6 sustain
          (hnr6), over 2 to 8 s of v7 (hnr7) and over the first v6-sustain-sized stretch after the splice (hnr7_same);
  vib     vibmod over the v7 sustain 1 to 10 s: pitch, level and harmonic 1 to 4 peak (3 to 8 Hz) amplitude and the 8 to 16 Hz peak.
Module use: measure(v6_path, v7_path, root, keep_ms) -> dict."""
import os, sys
import numpy as np
from scipy.signal import butter, sosfiltfilt
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "..", "samples"))
import lyria_notes_extract as le
import vibmod
import extend_v7 as ev
import v7_rng
import residual_check as rc


def env_db(x, sr, hop=0.005):
    w = int(hop * sr); n = len(x) // w
    return 20 * np.log10(np.maximum(np.sqrt((x[:n * w].reshape(n, w) ** 2).mean(1)), 1e-9))


def ripple(m, sr):
    e = env_db(m, sr)
    k = 60
    e = e - np.convolve(np.pad(e, (k // 2, k // 2), mode="edge"), np.ones(k) / k, mode="valid")[:len(e)]
    e = e[k // 2:-k // 2]
    ac = np.correlate(e, e, "full")[len(e) - 1:] / max(np.dot(e, e), 1e-12)
    return float(ac[8:61].max() * e.std())


def whiten(r, width=101):
    """r with its long term spectrum divided out (smoothed over `width` bins, about 0.3 Hz each at 5 s): what is left correlates with itself
    only through a real repeat in time, not through a few narrow strong spectral lines (a residual of a low note is mostly 60 to 100 Hz,
    which has few degrees of freedom in 100 ms, so even phase randomised noise of the same spectrum scores 0.7 on the raw recur)."""
    R = np.fft.rfft(r)
    p = np.convolve(np.abs(R) ** 2, np.ones(width) / width, "same")
    return np.fft.irfft(R / np.sqrt(np.maximum(p, 1e-30)), len(r))


def residual_bw(x, sr, f0, kmax=40):
    """residual_check.residual with the heterodyne bandwidth min(40 Hz, 0.35 f0): at 40 Hz the filters of notes under about 110 Hz
    (cello D2 at 73 Hz) leak into the next harmonic and the 'residual' keeps a harmonic."""
    t = np.arange(len(x)) / sr
    sos = butter(4, min(40.0, 0.35 * f0), fs=sr, output="sos")
    x = sosfiltfilt(butter(4, 30.0, btype="high", fs=sr, output="sos"), x)   # nothing under 30 Hz counts (the fresh noise has none: extend_v7.LOW_CUT_HZ)
    h = np.zeros_like(x)
    for k in range(1, kmax + 1):
        if k * f0 > 0.45 * sr:
            break
        z = x * np.exp(-2j * np.pi * k * f0 * t)
        z = sosfiltfilt(sos, z.real) + 1j * sosfiltfilt(sos, z.imag)
        h += 2 * (z * np.exp(2j * np.pi * k * f0 * t)).real
    return h, x - h


def measure(p6, p7, root, keep_ms=0.0):
    x6, sr = le.decode(p6); x7, _ = le.decode(p7)
    N0, N = len(x6), len(x7)
    ta, tr, xf = ev.zones(N0, sr, keep_ms / 1000.0)
    q = 2 ** 23 - 1
    out = {}
    out["null_head"] = float(np.abs(x7[:ta] - x6[:ta]).max() * q)
    out["null_tail"] = float(np.abs(x7[N - tr:] - x6[N0 - tr:]).max() * q)
    m7 = x7.mean(axis=1); m6 = x6.mean(axis=1)
    hop = max(0.010, 2.0 / vibmod.hz_of(root))   # an RMS window of at least two periods
    e10 = env_db(m7, sr, hop)
    z = np.abs(np.diff(e10))
    zs = [int((ta / sr - 0.1) / hop), int((ta + xf) / sr / hop + 1 + 0.1 / hop)]
    ze = [int((N - tr - xf) / sr / hop - 0.1 / hop), int((N - tr) / sr / hop + 0.1 / hop)]
    out["step_splice"] = float(max(z[zs[0]:zs[1]].max(), z[ze[0]:ze[1]].max()))
    out["step_all"] = float(z[:-30].max())
    f0 = vibmod.hz_of(root)
    f0 = vibmod.refine_f0(m7[int(1 * sr):int(2 * sr)], sr, f0)
    a, b = int(1.0 * sr), int(6.0 * sr)
    pad = int(0.5 * sr)   # the heterodyne filters ring for about 0.2 s at the ends of a segment: measure on the inside only
    h, r = residual_bw(m7[a - pad:b + pad], sr, f0)
    r = r[pad:-pad]
    out["recur"] = rc.recur(r, sr)
    out["recur_white"] = rc.recur(whiten(r), sr)
    R = np.fft.rfft(r)
    out["recur_chance"] = float(np.mean([rc.recur(np.fft.irfft(np.abs(R) * np.exp(2j * np.pi * v7_rng.stream("chance%d" % q).random(len(R))), len(r)), sr) for q in range(2)]))   # check tool only: a phase randomised copy of the same spectrum
    out["ripple"] = ripple(m7[a:b], sr)
    # harmonic to noise ratio as residual_check.residual (harmonics 1 to 40, 40 Hz heterodyne), edges of 0.1 s trimmed off
    def model_hnr(m2, i0, i1):
        pad = int(0.2 * sr)
        a0, a1 = max(i0 - pad, 0), min(i1 + pad, len(m2))
        seg = m2[a0:a1]
        f = vibmod.refine_f0(seg[:min(len(seg), int(1.5 * sr))], sr, f0)
        h, r = residual_bw(seg, sr, f)
        t0, t1 = i0 - a0, len(seg) - (a1 - i1)
        return 10 * np.log10(np.dot(h[t0:t1], h[t0:t1]) / np.dot(r[t0:t1], r[t0:t1]))
    i0 = max(int(0.25 * N0), ta); i1 = min(int(0.9 * N0), N0 - tr)
    if i1 - i0 < int(0.2 * sr):
        i0, i1 = max(int(0.2 * N0), int(0.1 * sr)), N0 - int(0.05 * sr)
    out["hnr6"] = model_hnr(m6, i0, i1)
    out["hnr7"] = model_hnr(m7, int(2 * sr), int(8 * sr))
    out["hnr7_same"] = model_hnr(m7, ta + xf + int(0.3 * sr), ta + xf + int(0.3 * sr) + (i1 - i0))
    def vib(seg):
        v = vibmod.measure_note(seg, sr, vibmod.hz_of(root))
        d = dict(pitch_depth=v["pitch"][4], level_depth=v["level"][4], harm_depth=max(h_[4] for h_ in v["harm"]),
                 ratio=max([v["pitch"][3], v["level"][3]] + [h_[3] for h_ in v["harm"]]),
                 ratio2=max([v["level2"][1]] + [h_[1] for h_ in v["harm2"]]),
                 pitch_pp=float(np.ptp(v["pitch_c"][20:-20])), level_pp=float(np.ptp(v["level_db"][20:-20])),
                 harm_pp=float(max(np.ptp(hh[20:-20]) for hh in v["harm_db"])))
        return d
    out["vib6"] = vib(m6[i0:i1])
    out["vib7"] = vib(m7[int(1.0 * sr):int(10 * sr)])
    return out


if __name__ == "__main__":
    o = measure(sys.argv[1], sys.argv[2], int(sys.argv[3]), float(sys.argv[4]) if len(sys.argv) > 4 else 0.0)
    for k, v in o.items():
        print(k, v)
