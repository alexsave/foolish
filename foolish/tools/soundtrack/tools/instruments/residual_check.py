#!/usr/bin/env python3
"""residual_check.py WAV SECTIONS.txt : echo and noise check on the held notes of the OFF halves of the on-then-off demo.
For each OFF note (2.4 s from 0.3 s after the start) the harmonic part (harmonics 1 to 40, 40 Hz heterodyne bandwidth around the
note's refined f0) is subtracted; what is left is the residual (bow, breath, noise, the non-harmonic part). Reported per instrument,
median over notes: HNR (harmonic power over residual power, dB); recur = share of 100 ms windows of the residual (hop 50 ms) whose
best normalised cross-correlation with the residual 100 to 1200 ms earlier is above 0.5 (random noise gives about 0; a looped
stretch that repeats the same noise burst gives high values: the 'ghost repeat'); and the same for the envelope of the whole signal."""
import os, sys
import numpy as np
from scipy.signal import butter, sosfiltfilt
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "..", "samples"))
import lyria_notes_extract as le
import vibmod, vib_demo_measure as dm


def residual(x, sr, f0, kmax=40):
    t = np.arange(len(x)) / sr
    sos = butter(4, 40.0, fs=sr, output="sos")
    h = np.zeros_like(x)
    for k in range(1, kmax + 1):
        if k * f0 > 0.45 * sr:
            break
        z = x * np.exp(-2j * np.pi * k * f0 * t)
        z = sosfiltfilt(sos, z.real) + 1j * sosfiltfilt(sos, z.imag)
        h += 2 * (z * np.exp(2j * np.pi * k * f0 * t)).real
    return h, x - h


def recur(r, sr, win=0.100, hop=0.050, lo=0.100, hi=1.000, thr=0.5):
    W, H = int(win * sr), int(hop * sr)
    hits, n = 0, 0
    lags = np.arange(int(lo * sr), int(hi * sr), 24)
    for a in range(int(hi * sr), len(r) - W, H):
        seg = r[a:a + W]
        e = np.dot(seg, seg)
        if e < 1e-14:
            continue
        best = 0.0
        for L in lags:
            p = r[a - L:a - L + W]
            c = np.dot(seg, p) / np.sqrt(e * np.dot(p, p) + 1e-30)
            if c > best:
                best = c
        hits += best > thr
        n += 1
    return hits / max(n, 1)


def main():
    wav, sec = sys.argv[1:3]
    x, sr = le.decode(wav)
    m = x.mean(axis=1)
    res = {}
    for line in open(sec):
        p = line.split()
        if len(p) < 5 or p[5] != "off":
            continue
        t0, inst = float(p[0]), p[3]
        for i, nm in enumerate(dm.LONGS[inst]):
            a = t0 + i * 4 * 60 / dm.BPM + 0.3
            seg = m[int(a * sr):int((a + 2.0) * sr)]
            f0 = vibmod.refine_f0(seg[:int(0.8 * sr)], sr, dm.hz(dm.midi(nm)))
            h, r = residual(seg, sr, f0)
            hnr = 10 * np.log10(np.dot(h, h) / max(np.dot(r, r), 1e-20))
            res.setdefault(inst, []).append((hnr, recur(r, sr)))
    for inst, v in res.items():
        v = np.array(v)
        print("%-8s HNR median %.1f dB | residual recur share median %.2f (worst %.2f)" % (inst, np.median(v[:, 0]), np.median(v[:, 1]), v[:, 1].max()))


if __name__ == "__main__":
    main()
