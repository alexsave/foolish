"""Fit of the relaxation loss (iteration 1 richness, F2): amplitude decay rate of a partial of frequency f,
   alpha(f) = sigma0 + sum_i  a_i  x_i^2 / (1 + x_i^2),  x_i = f / f_i      (altstring.h: standard linear solid terms, f_i = 1 / (2 pi tau_i))
to the target: the old curve (sigma0 + sigma1 (w/c)^2, Q 300 at 2 kHz) up to 500 Hz, a constant Q from 1 kHz on, a log-log blend between.
    python -I charts/loss_fit.py [NREL]      prints f_i, a_i * Q (1/s) and the fit error"""
import sys
import numpy as np
from scipy.optimize import least_squares
NREL = int(sys.argv[1]) if len(sys.argv) > 1 else 2
S0, QN = 1.2, 300.0
FMAX = float(sys.argv[2]) if len(sys.argv) > 2 else 12000.0  # the highest relaxation corner: a corner above the audio band is a Kelvin-Voigt k^2 term in disguise
f = np.geomspace(100.0, 12000.0, 120)
old = S0 + (np.pi * 2000.0 / QN - S0) * (f / 2000.0) ** 2
flat = np.pi * f / QN
w = np.clip((np.log(f) - np.log(500.0)) / (np.log(1000.0) - np.log(500.0)), 0, 1)
tgt = np.exp((1 - w) * np.log(old) + w * np.log(np.maximum(flat, S0 * 1.001)))


def model(p, f):
    fi = np.exp(p[:NREL])
    a = np.exp(p[NREL:])
    out = S0 + np.zeros_like(f)
    for i in range(NREL):
        x2 = (f / fi[i]) ** 2
        out += a[i] * x2 / (1 + x2)
    return out


best = None
rng = np.random.default_rng(1)
for t in range(200):
    p0 = np.concatenate([np.log(np.sort(rng.uniform(800, 9000, NREL))), np.log(rng.uniform(5, 60, NREL))])
    lo = np.concatenate([np.log(np.full(NREL, 500.0)), np.log(np.full(NREL, 0.1))])
    hi = np.concatenate([np.log(np.full(NREL, FMAX)), np.log(np.full(NREL, 1e4))])
    r = least_squares(lambda p: np.log(model(p, f)) - np.log(tgt), np.clip(p0, lo + 1e-9, hi - 1e-9), bounds=(lo, hi))
    if best is None or r.cost < best.cost:
        best = r
p = best.x
fi, a = np.exp(p[:NREL]), np.exp(p[NREL:])
o = np.argsort(fi)
print("NREL", NREL, "corners f_i Hz", fi[o], " a_i (1/s) at Q 300", a[o], " a_i*Q", a[o] * QN, "rms log error dB", 20 * np.sqrt(2 * best.cost / len(f)) / np.log(10) * np.log(10) / 2.302585 * 1.0)
res = 20 * np.log10(model(p, f) / tgt)
print("max |dB error|", np.abs(res).max(), "at f", f[np.argmax(np.abs(res))])
for ff in (100, 250, 500, 800, 1000, 2000, 4000, 8000, 10000):
    k = np.argmin(abs(f - ff))
    print("%6d Hz  target alpha %7.2f  fit %7.2f  Q fit %6.0f" % (f[k], tgt[k], model(p, f)[k], np.pi * f[k] / model(p, f)[k]))
