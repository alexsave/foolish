#!/usr/bin/env python3
"""vib_stats.py INSTRUMENT: vibrato rate (Hz), depth (cents, peak = half the peak-to-peak) and tremolo (dB peak-to-peak at that rate) of the
stored samples of /Users/alex/Dev/soundtrack-samples/lyria/<instrument>/ over 20 to 90 percent of each sample (samples of 0.9 s or more).
The vibrato is the largest 4 to 8 Hz peak of the f0 curve; the tremolo is the level spectrum's size at the same frequency (+-0.6 Hz)."""
import sys, glob, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mod_spectrum as ms


def main():
    inst = sys.argv[1]
    rate, depth, trem = [], [], []
    for f in sorted(glob.glob("/Users/alex/Dev/soundtrack-samples/lyria/%s/*.wav" % inst)):
        x, sr = ms.le.decode(f)
        n = len(x) / sr
        if n < 0.9:
            continue
        m = x.mean(axis=1)
        r = ms.curves(m, sr, 0.2 * n, 0.9 * n)
        c, t, lv, vs = r
        full = ms.spec(c, lo=4.0, hi=8.0)
        if not full:
            continue
        f0_, pp = full[0]
        lp = [a for fq, a in ms.spec(lv, lo=f0_ - 0.6, hi=f0_ + 0.6)] or [0.0]
        rate.append(f0_); depth.append(pp / 2); trem.append(lp[0])
        print("%s\trate %.2f Hz\tdepth %.1f c peak\ttremolo %.2f dB pp" % (os.path.basename(f), f0_, pp / 2, lp[0]))
    print("%s: %d samples; median rate %.2f Hz (quartiles %.2f-%.2f), depth %.1f cents peak (%.1f-%.1f), tremolo %.2f dB pp (%.2f-%.2f)" % (
        inst, len(rate), np.median(rate), np.percentile(rate, 25), np.percentile(rate, 75), np.median(depth), np.percentile(depth, 25),
        np.percentile(depth, 75), np.median(trem), np.percentile(trem, 25), np.percentile(trem, 75)))


main()
