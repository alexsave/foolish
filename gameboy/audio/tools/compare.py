#!/usr/bin/env python3
# compare.py - hold each Game Boy render to the real take it was made from.
#
#   python3 -I tools/compare.py BUILD > BUILD/compare.tsv
#
# For every card cue's take: the target (BUILD/target, the real take's window
# at the Game Boy's level), approach A at 60 Hz (gb_a, the driver's rate) and at 241 Hz (gb_a241),
# approach B (gb_b). The measures:
#   level   loudest 50 ms RMS, render minus target, dB
#   env     the 5 ms envelopes' mean absolute difference, dB, over the frames
#           where the target is within 30 dB of its peak, after matching the
#           loudest 50 ms and the best lag within 10 ms (the shape, not the level)
#   lo, hi  the 16-band spectra's mean absolute difference, dB, both normalised
#           to their total, over bands within 30 dB of the target's loudest,
#           below 4 kHz (lo) and from 4 kHz (hi)
# No randomness.
import sys, os
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyse import read_wav  # noqa: E402

EDGES = np.geomspace(60, 15000, 17)
CENTRES = np.sqrt(EDGES[:-1] * EDGES[1:])


def env5(x, sr):
    n = int(sr * 0.005)
    k = len(x) // n
    return 20 * np.log10(np.sqrt(np.mean(x[: k * n].reshape(k, n) ** 2, axis=1)) + 1e-9)


def loud50(x, sr):
    n = int(sr * 0.05)
    if len(x) < n:
        return 20 * np.log10(np.sqrt(np.mean(x * x)) + 1e-12)
    return 20 * np.log10(np.sqrt(np.max(np.convolve(x * x, np.ones(n) / n, mode="valid"))) + 1e-12)


def bands(x, sr):
    n = 1 << int(np.ceil(np.log2(len(x))))
    X = np.abs(np.fft.rfft(x, n)) ** 2
    f = np.fft.rfftfreq(n, 1 / sr)
    b = np.array([X[(f >= lo) & (f < hi)].sum() for lo, hi in zip(EDGES[:-1], EDGES[1:])]) + 1e-20
    return 10 * np.log10(b / b.sum())


def measure(t, ts, r, rs):
    lv = loud50(r, rs) - loud50(t, ts)
    et, er = env5(t, ts), env5(r, rs) - lv
    best = 1e9
    for lag in range(-2, 3):
        a = et
        b = np.roll(np.pad(er, (0, max(0, len(et) - len(er))), constant_values=-120)[: len(et)], lag)
        m = a > a.max() - 30
        best = min(best, float(np.mean(np.abs(np.maximum(a[m], a.max() - 40) - np.maximum(b[m], a.max() - 40)))))
    bt, br = bands(t, ts), bands(r, rs)
    m = bt > bt.max() - 30
    lo = m & (CENTRES < 4000)
    hi = m & (CENTRES >= 4000)
    d = np.abs(bt - np.maximum(br, bt.max() - 40))
    return lv, best, float(np.mean(d[lo])) if lo.any() else float("nan"), float(np.mean(d[hi])) if hi.any() else float("nan")


def main():
    b = sys.argv[1]
    print("take\tA_level\tA_env\tA_lo\tA_hi\tA241_level\tA241_env\tA241_lo\tA241_hi\tB_level\tB_env\tB_lo\tB_hi")
    for name in sorted(os.listdir(os.path.join(b, "target"))):
        ts, t = read_wav(os.path.join(b, "target", name))
        row = [name[:-4]]
        for d in ("gb_a", "gb_a241", "gb_b"):
            p = os.path.join(b, d, name)
            if not os.path.exists(p):
                row += ["", "", "", ""]
                continue
            rs, r = read_wav(p)
            row += ["%.1f" % v for v in measure(t, ts, r, rs)]
        print("\t".join(row))


if __name__ == "__main__":
    main()
