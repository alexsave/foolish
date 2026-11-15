#!/usr/bin/env python3
"""hnr_source_v7.py INSTRUMENT : harmonic to noise ratio (check_v7.residual_bw, 25 to 90 percent of the file, edges padded and trimmed) of every
Lyria SOURCE cut /Users/alex/Dev/soundtrack-samples/lyria/<inst>/ next to the v6 and v7 values of the check table
/Users/alex/Music/foolish_cities_v5/analysis/lyria_samples/<inst>_flat_v7_checks.tsv. Prints one line per sample and the median differences.
The source has vibrato, so a heterodyne at one fixed f0 loses the upper harmonics: its HNR reads lower than the flat sets by construction."""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "..", "samples"))
import lyria_notes_extract as le
import vibmod, check_v7 as cv

SAMP = "/Users/alex/Dev/soundtrack-samples/lyria"
REP = "/Users/alex/Music/foolish_cities_v5/analysis/lyria_samples"


def main():
    inst = sys.argv[1]
    rows = [l.rstrip("\n").split("\t") for l in open(os.path.join(REP, inst + "_flat_v7_checks.tsv"))]
    hdr = rows[0]
    d6, d7 = [], []
    for r in rows[1:]:
        fn = r[0]
        root = int(fn.split("_")[2].split(".")[0])
        x, sr = le.decode(os.path.join(SAMP, inst, fn))
        m = x.mean(axis=1)
        i0, i1 = int(0.25 * len(m)), int(0.9 * len(m))
        pad = int(0.2 * sr)
        a0, a1 = max(i0 - pad, 0), min(i1 + pad, len(m))
        f = vibmod.refine_f0(m[a0:a1][:int(1.5 * sr)], sr, vibmod.hz_of(root))
        h, rr = cv.residual_bw(m[a0:a1], sr, f)
        t0, t1 = i0 - a0, len(h) - (a1 - i1)
        hs = 10 * np.log10(np.dot(h[t0:t1], h[t0:t1]) / np.dot(rr[t0:t1], rr[t0:t1]))
        h6, h7 = float(r[hdr.index("hnr6")]), float(r[hdr.index("hnr7")])
        d6.append(h6 - hs); d7.append(h7 - hs)
        print("%-28s source %.1f v6 %.1f v7 %.1f" % (fn, hs, h6, h7))
    print("%s: median v6 - source %.1f dB, v7 - source %.1f dB, v7 - v6 median %.1f max %.1f min %.1f" % (inst, np.median(d6), np.median(d7),
          np.median(np.array(d7) - np.array(d6)), np.max(np.array(d7) - np.array(d6)), np.min(np.array(d7) - np.array(d6))))


if __name__ == "__main__":
    main()
