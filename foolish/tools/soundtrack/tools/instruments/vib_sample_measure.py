#!/usr/bin/env python3
"""vib_sample_measure.py INSTRUMENT SUFFIX [DIR] : vibmod measures of every sample of samples/manifests/lyria_<inst><SUFFIX>.txt
over its 25 to 90 percent stretch (SUFFIX '' = source cuts, '_flat' = v5, '_flat_v6' = v6). Prints one row per file: pitch pp (cents),
level pp (dB), the largest of harmonic 1 to 4 pp (dB) with the 3 to 8 Hz peak/floor ratios, and the medians at the end.
Module use: measure_file(path, root) -> vibmod dict."""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "samples"))
import lyria_notes_extract as le
import vibmod

MAN = os.path.join(HERE, "..", "..", "samples", "manifests")
SAMPLES = "/Users/alex/Dev/soundtrack-samples"


def measure_file(path, root, lo=0.25, hi=0.90):
    x, sr = le.decode(path)
    m = x.mean(axis=1)
    seg = m[int(lo * len(m)):int(hi * len(m))]
    return vibmod.measure_note(seg, sr, vibmod.hz_of(root) if hasattr(vibmod, "hz_of") else 440.0 * 2 ** ((root - 69) / 12.0)), len(m) / sr


def rows(inst, suffix):
    out = []
    for l in open(os.path.join(MAN, "lyria_%s%s.txt" % (inst, suffix))):
        if not l.strip():
            continue
        root, layer, rr, path = l.split(None, 3)
        path = path.strip().replace("/SAMPLES/soundtrack/samples/", SAMPLES + "/")
        out.append((int(root), os.path.basename(path), path))
    return out


def main():
    inst, suffix = sys.argv[1], sys.argv[2]
    tot = []
    for root, fn, path in rows(inst, suffix):
        m, dur = measure_file(path, root)
        hp = max(h[4] for h in m["harm"])
        hr = max(h[3] for h in m["harm"])
        tot.append((m["pitch"][4], m["pitch"][3], m["level"][4], m["level"][3], hp, hr))
        print("%-26s %4.2fs pitch %6.2f c (%5.1f)  level %5.2f dB (%5.1f)  harm %5.2f dB (%5.1f)" % (fn, dur, *tot[-1]))
    t = np.median(np.array(tot), axis=0)
    print("MEDIAN %s%s: pitch %.2f c (%.1f), level %.2f dB (%.1f), harm %.2f dB (%.1f)" % (inst, suffix, *t))


if __name__ == "__main__":
    main()
