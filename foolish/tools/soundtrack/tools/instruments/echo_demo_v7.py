#!/usr/bin/env python3
"""echo_demo_v7.py WAV SECTIONS.txt : residual_check.py for the OFF halves of the on-then-off demo, with three recurrence numbers per instrument
(median over the 5 held notes, 2.0 s from 0.3 s into the note, residual = harmonics removed with the heterodyne bandwidth min(40 Hz, 0.35 f0)):
  raw     share of 100 ms windows whose best correlation with the residual 0.1 to 1.0 s earlier is over 0.5 (residual_check.recur);
  chance  the same on a phase randomised copy of the same residual (same spectrum, no repeated time structure): the level a fresh noise of this
          spectrum reaches (a low note's residual is mostly 60 to 100 Hz, few degrees of freedom, so chance is high there);
  white   the same after the residual's long term spectrum is divided out (check_v7.whiten): a repeat in time is all that is left.
Also HNR (as residual_check) and the repeated-envelope ripple (artifact_check style, 40 to 300 ms lags)."""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "..", "samples"))
import lyria_notes_extract as le
import vibmod, vib_demo_measure as dm, residual_check as rc, check_v7 as cv, v7_rng


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
            pad = int(0.4 * sr)
            seg = m[int(a * sr) - pad:int((a + 2.0) * sr) + pad]
            f0 = vibmod.refine_f0(seg[pad:pad + int(0.8 * sr)], sr, dm.hz(dm.midi(nm)))
            h, r = cv.residual_bw(seg, sr, f0)
            r = r[pad:-pad]
            h = h[pad:-pad]
            R = np.fft.rfft(r)
            ch = rc.recur(np.fft.irfft(np.abs(R) * np.exp(2j * np.pi * v7_rng.stream("demo%s%d" % (inst, i)).random(len(R))), len(r)), sr)
            res.setdefault(inst, []).append((10 * np.log10(np.dot(h, h) / np.dot(r, r)), rc.recur(r, sr), ch, rc.recur(cv.whiten(r), sr), cv.ripple(seg[pad:-pad], sr)))
    for inst, v in res.items():
        v = np.array(v)
        print("%-8s HNR %.1f dB | recur raw %.2f chance %.2f white %.2f (worst white %.2f) | envelope repeat ripple %.2f dB" % (inst, np.median(v[:, 0]), np.median(v[:, 1]), np.median(v[:, 2]), np.median(v[:, 3]), v[:, 3].max(), np.median(v[:, 4])))


if __name__ == "__main__":
    main()
