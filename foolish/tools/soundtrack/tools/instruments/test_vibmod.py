#!/usr/bin/env python3
"""test_vibmod.py: vibmod must read zero on a pure steady tone, and the right depth and rate on a tone with known FM and AM.
Run: python3 -I test_vibmod.py   (prints PASS). Mutation check: see MUTATIONS in the file header of the commit message."""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vibmod

SR = 48000


def tone(f0, dur, fm_c=0.0, fm_hz=5.0, am_db=0.0, harm_am_db=0.0):
    t = np.arange(int(dur * SR)) / SR
    ph0 = 2 * np.pi * f0 * np.cumsum(2 ** (fm_c * np.sin(2 * np.pi * fm_hz * t) / 1200.0)) / SR
    g = 10 ** (am_db / 2 * np.sin(2 * np.pi * fm_hz * t) / 20.0)
    y = np.zeros_like(t)
    for k, a in ((1, 1.0), (2, 0.5), (3, 0.3), (4, 0.2)):
        gk = 10 ** (harm_am_db / 2 * np.sin(2 * np.pi * fm_hz * t) / 20.0) if k == 2 else 1.0
        y += a * gk * np.sin(k * ph0)
    return y * g


def main():
    z = vibmod.measure_note(tone(220.0, 2.0), SR, 220.0 * 1.01)
    assert z["pitch"][4] < 0.05 and z["pitch"][3] < 3, z["pitch"]
    assert z["level"][4] < 0.01, z["level"]
    fm = vibmod.measure_note(tone(220.0, 2.0, fm_c=7.0), SR, 220.0)
    assert abs(fm["pitch"][4] - 14.0) < 1.0 and abs(fm["pitch"][1] - 5.0) < 0.4 and fm["pitch"][3] > 20, fm["pitch"]
    assert fm["level"][4] < 0.1, fm["level"]
    am = vibmod.measure_note(tone(220.0, 2.0, am_db=0.5), SR, 220.0)
    assert abs(am["level"][4] - 0.5) < 0.1 and am["level"][3] > 10 and am["pitch"][4] < 0.2, (am["level"], am["pitch"])
    hm = vibmod.measure_note(tone(220.0, 2.0, harm_am_db=3.0), SR, 220.0)
    assert hm["harm"][1][4] > 2.5 and hm["harm"][0][4] < 0.1, hm["harm"]
    print("PASS")


if __name__ == "__main__":
    main()
