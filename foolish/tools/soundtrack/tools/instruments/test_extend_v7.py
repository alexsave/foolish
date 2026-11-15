#!/usr/bin/env python3
"""test_extend_v7.py: a synthetic 1.8 s stereo cello-like sample (A3, 8 steady harmonics with fixed phases, plus a noise that REPEATS every
0.3 s, the loop echo) goes through extend_v7.extend. Checks:
  1 length: 11.9 to 12.1 s;
  2 null: the kept attack and the moved release tail are identical to the input (under 1e-9);
  3 echo: the residual recurrence share (residual_check.recur, lags 0.1 to 1.0 s) is high on the input (over 0.5) and under 0.15 after;
  4b harmonic phase of 1 to 4 equal before and after each splice (under 0.1 rad: the tail is moved by whole periods);
  4 fresh: no window of the fresh noise correlates over 0.35 with any same length window of the input residual (never a copy);
  5 drift: pitch drift std 0.03 to 0.2 cent (ev info), vibmod 3 to 8 Hz peak/floor ratios of pitch, level and harmonic 1 to 4 under 3,
    level 3 to 8 Hz depth under 0.05 dB;
  6 seed: the same name gives the same output, another name another noise;
  7 no stray random: only v7_rng.py may mention numpy.random, random, default_rng, time or Date.
Run: python3 -I test_extend_v7.py   (prints PASS).
Mutations checked (each must fail the named check): 3 and 4 the fresh noise replaced by the tiled measured residual; 2 and 4b the tail moved by a
quarter period more (D += quarter period, f not re-derived); 5 DRIFT_PITCH_C 0.1 -> 3 and DRIFT_HZ 0.8 -> 5; 6 stream() ignoring the name; 7 a np.random call added."""
import os, re, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "..", "samples"))
import extend_v7 as ev
import residual_check as rc
import vibmod
import v7_rng

SR = 44100


def source():
    rng = v7_rng.stream("test_source")
    n = int(1.8 * SR)
    t = np.arange(n) / SR
    f = 220.0
    ph = [0.3, 1.1, 2.0, 0.7, 1.9, 2.6, 0.2, 1.4]
    y = sum(a * np.sin(2 * np.pi * (k + 1) * f * t + p) for k, (a, p) in enumerate(zip([1, .6, .45, .3, .2, .15, .1, .08], ph)))
    env = np.minimum(t / 0.05, 1) * np.minimum((1.8 - t) / 0.1, 1)
    pat = rng.standard_normal((2, int(0.3 * SR)))
    pat = np.stack([np.convolve(p, np.ones(8) / 8, "same") for p in pat])      # coloured
    noise = np.tile(pat, (1, 7))[:, :n].T * 0.08
    return (np.stack([y, 0.9 * y], 1) * 0.15 + noise * 0.15) * env[:, None]


def main():
    x = source()
    y, info = ev.extend(x, SR, 57, "test_a.wav")
    N0, ta, tr = len(x), info["ta"], info["tr"]
    assert 11.9 < len(y) / SR < 12.1, len(y) / SR
    assert np.abs(y[:ta] - x[:ta]).max() < 1e-9
    assert np.abs(y[len(y) - tr:] - x[N0 - tr:]).max() < 1e-9, np.abs(y[len(y) - tr:] - x[N0 - tr:]).max()
    rin = info["r0"][:, 0]
    rout = info["rn"][:, 0]
    b = rc.recur(rin[int(0.2 * SR):], SR)
    a = rc.recur(rout[int(1.0 * SR):int(5.0 * SR)], SR)
    assert b > 0.5 and a < 0.15, (b, a)
    # fresh: correlation of 100 ms windows of the fresh noise (3 to 9 s) with all windows of the source residual
    W = int(0.1 * SR)
    worst = 0.0
    src = rin[int(0.45 * SR):int(1.5 * SR)]
    for a0 in range(int(3 * SR), int(9 * SR), int(0.5 * SR)):
        w = rout[a0:a0 + W]
        c = np.correlate(src, w, "valid") / np.sqrt(np.convolve(src ** 2, np.ones(W), "valid") * np.dot(w, w))
        worst = max(worst, float(np.abs(c).max()))
    assert worst < 0.35, worst
    # the harmonics are one continuous periodic tone through both splices: the phase of harmonics 1 to 4 (absolute time) just before the
    # tail and just after it, and in the kept attack and after the head splice, agree
    f, ks4 = info["f0"], [1, 2, 3, 4]
    t0, a1 = N0 - tr + info["D"], info["a1"]
    def ph(i0, i1):
        return np.angle(ev.project(y, SR, f, i0, i1, ks4)[0])
    for (u0, u1), (v0, v1) in (((int(0.1 * SR), int(0.3 * SR)), (a1 + int(0.15 * SR), a1 + int(0.35 * SR))), ((t0 - int(0.4 * SR), t0 - int(0.2 * SR)), (t0 + int(0.02 * SR), t0 + int(0.12 * SR)))):
        d = np.abs(np.angle(np.exp(1j * (ph(u0, u1) - ph(v0, v1)))))
        assert d.max() < 0.1, ("harmonic phase jump at a splice", d)
    assert 0.03 < info["cents_std"] < 0.2, info["cents_std"]
    seg = y[int(1.0 * SR):int(10 * SR)].mean(axis=1)
    v = vibmod.measure_note(seg, SR, vibmod.hz_of(57))
    assert v["pitch"][3] < 3 and v["level"][3] < 3 and max(hh[3] for hh in v["harm"]) < 3, (v["pitch"], v["level"], [hh[3] for hh in v["harm"]])
    assert v["level"][4] < 0.05, v["level"]
    y2, _ = ev.extend(x, SR, 57, "test_a.wav")
    y3, _ = ev.extend(x, SR, 57, "test_b.wav")
    assert np.array_equal(y, y2) and not np.allclose(y[int(3 * SR):int(4 * SR)], y3[int(3 * SR):int(4 * SR)])
    bad = re.compile(r"numpy\.random|np\.random|default_rng|import random|random\.|time\.time|Date\.now|rand\(")
    for fn in ("extend_v7.py", "extend_set_v7.py", "check_v7.py"):
        for i, l in enumerate(open(os.path.join(HERE, fn))):
            assert not bad.search(l), "%s:%d stray random: %s" % (fn, i + 1, l.strip())
    print("PASS recur in %.2f out %.2f, fresh worst corr %.2f, drift %.3f c, ratios pitch %.2f level %.2f" % (b, a, worst, info["cents_std"], v["pitch"][3], v["level"][3]))


if __name__ == "__main__":
    main()
