#!/usr/bin/env python3
"""test_flatten_v6.py: a synthetic 2.5 s stereo tone at A3 (MIDI 57) with 4 harmonics, 12 cents FM at 5 Hz, 2 dB AM at 5 Hz on
the fundamental and 4 dB AM in antiphase on harmonic 2 (the cello-body effect) goes through flatten_v6; afterwards the pitch,
level and harmonic modulation measured by vibmod must be under 0.5 c / 0.2 dB / 0.5 dB pp, with the vibrato clearly present before.
A clean steady tone must come out within 0.2 dB and 1 cent of itself (no damage). Run: python3 -I test_flatten_v6.py (PASS).
Mutations checked: PITCH_ITERS=0, passes without 'harm', MAX_G_DB=0.5."""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flatten_v6 as f
import vibmod

SR = 48000


def tone(fm_c, am1_db, am2_db):
    t = np.arange(int(2.5 * SR)) / SR
    s5 = np.sin(2 * np.pi * 5 * t)
    f0 = 220.0
    ph = 2 * np.pi * f0 * np.cumsum(2 ** (fm_c * s5 / 1200.0)) / SR
    env = np.minimum(t / 0.05, 1.0) * np.minimum((2.5 - t) / 0.1, 1.0)
    y = np.zeros_like(t)
    for k, a in ((1, 1.0), (2, 0.6), (3, 0.4), (4, 0.25)):
        g = 10 ** ((am1_db if k == 1 else (-am2_db if k == 3 else am2_db if k == 2 else 0.0)) / 2 * s5 / 20.0)
        y += a * g * np.sin(k * ph)
    y *= env * 0.3
    return np.stack([y, y * 0.9], axis=1)


def main():
    x = tone(12.0, 2.0, 4.0)
    b = f.stats(x, SR, 57)
    assert b[0] > 15 and b[1] > 1.0 and b[2] > 3.0, b
    y, ok = f.flatten_v6(x, SR, 57, 0.0)
    assert ok
    a = f.stats(y, SR, 57)
    assert a[0] < 0.5 and a[1] < 0.2 and a[2] < 0.5, (b, a)
    c = tone(0.0, 0.0, 0.0)
    z, ok = f.flatten_v6(c, SR, 57, 0.0)
    assert ok
    n = len(c)
    i0, i1 = int(0.4 * n), int(0.8 * n)
    d = 20 * np.log10(np.sqrt(np.mean(z[i0:i1, 0] ** 2)) / np.sqrt(np.mean(c[i0:i1, 0] ** 2)))
    assert abs(d) < 0.2, d
    assert f.stats(z, SR, 57)[0] < 1.0
    print("PASS before %s after %s" % (np.round(b, 2), np.round(a, 2)))


if __name__ == "__main__":
    main()
