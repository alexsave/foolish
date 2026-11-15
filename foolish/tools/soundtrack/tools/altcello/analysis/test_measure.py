#!/usr/bin/env python3
"""test_measure.py: synthetic signals with known answers for measure.py.
Run: nice -n 15 /Users/alex/Dev/soundtrack-tools/venv/bin/python -I test_measure.py
"""
import os, sys, math, unittest
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure as M

SR = M.SR
SIL = 0.3


def tone(f0, dur, amps, vib_hz=0.0, vib_c=0.0, B=0.0, attack=0.0, noise_pow=0.0, seed=1):
    n = int(dur * SR); t = np.arange(n) / SR
    fs = f0 * 2 ** (vib_c / 1200.0 * np.sin(2 * np.pi * vib_hz * t)) if vib_hz else np.full(n, f0)
    ph = 2 * np.pi * np.cumsum(fs) / SR; y = np.zeros(n)
    for k, a in enumerate(amps, 1):
        y += a * np.sin(k * ph * math.sqrt(1 + B * k * k))
    if attack > 0: y *= np.minimum(1.0, t / attack)
    if noise_pow > 0: y += np.random.default_rng(seed).normal(0, math.sqrt(noise_pow), n)
    return np.concatenate([np.zeros(int(SIL * SR)), y, np.zeros(int(SIL * SR))])


def run(y, midi, dur):
    return M.analyze_signal(y, SR, [(SIL, dur, midi)], "synthetic")["S"]


class T(unittest.TestCase):
    def test_vibrato(self):
        S = run(tone(110.0, 3.0, [1 / k for k in range(1, 9)], vib_hz=5.5, vib_c=20.0), 45, 3.0)
        self.assertLess(abs(S["vib_rate_hz"] - 5.5), 0.2)
        self.assertLess(abs(S["vib_depth_c"] - 20.0), 3.0)
        self.assertLess(S["pitch_micro_c"], 2.0)       # vibrato removed from the micro-variation

    def test_harmonic_slope(self):
        S = run(tone(196.0, 1.5, [1 / k for k in range(1, 13)]), 55, 1.5)     # 1/k = -6 dB per octave
        self.assertLess(abs(S["h2"] - (-6.02)), 1.0)
        self.assertLess(abs(S["h4"] - (-12.04)), 1.0)

    def test_hnr(self):
        amps = [1 / k for k in range(1, 9)]; hp = sum(a * a / 2 for a in amps)
        for hnr in (10.0, 20.0):
            S = run(tone(220.0, 2.0, amps, noise_pow=hp / 10 ** (hnr / 10)), 57, 2.0)
            self.assertLess(abs(S["hnr_res_db"] - hnr), 2.0, "residual HNR at %g" % hnr)
            self.assertLess(abs(S["hnr_db"] - hnr), 2.0, "autocorrelation HNR at %g" % hnr)

    def test_attack(self):
        S = run(tone(220.0, 1.5, [1.0, 0.5, 0.25], attack=0.100), 57, 1.5)
        self.assertLess(abs(S["attack_ms"] - 100.0), 20.0)
        self.assertLess(abs(S["attack_10_90_ms"] - 80.0), 20.0)

    def test_inharmonic(self):
        B = 1e-3
        S = run(tone(150.0, 2.0, [1.0 / k for k in range(1, 13)], B=B), 50, 2.0)
        want = 1200 * math.log2(math.sqrt((1 + B * 100) / (1 + B)))              # partial 10 vs 10 x partial 1
        self.assertLess(abs(S["dev10_c"] - want), 2.0)
        self.assertLess(abs(S["dev5_c"] - 1200 * math.log2(math.sqrt((1 + B * 25) / (1 + B)))), 2.0)

    def test_cli_and_compare_figure(self):
        import tempfile, subprocess, wave
        d = tempfile.mkdtemp(); y = tone(220.0, 1.5, [1 / k for k in range(1, 9)], attack=0.05)
        w = wave.open(d + "/t.wav", "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes((y * 20000).astype("<i2").tobytes()); w.close()
        open(d + "/n.txt", "w").write("# start dur midi\n%g 1.5 57 extra\n" % SIL)
        subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "measure.py"), "--wav", d + "/t.wav", "--notes", d + "/n.txt",
                        "--label", "ALT", "--out", d + "/alt"], check=True)
        rows = open(d + "/alt.tsv").read().splitlines()
        self.assertEqual(rows[0], "metric\tvalue\tunit\tsource"); self.assertTrue(any(r.startswith("h2\t") for r in rows))
        with open(d + "/tgt.tsv", "w") as f:
            f.write("metric\tvalue\tunit\tsource\tspread_min\tspread_max\n")
            for r in rows[1:]: f.write(r.replace("\tALT", "\tTARGET") + "\t\t\n")
        M.compare_figure(d + "/alt", d + "/tgt.tsv", "test", d + "/c.png")
        self.assertGreater(os.path.getsize(d + "/c.png"), 20000)


if __name__ == "__main__":
    unittest.main(verbosity=2)
