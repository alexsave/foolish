"""Fit of the relaxation loss with the DISCRETE scheme (altstring.c): the decay rate of mode p comes from the root z of the scheme's characteristic equation
   (1 + s0 k) z - 2 + (1 - s0 k) / z + k^2 (c^2 L_p + kap^2 L_p^2) + 2 k L_p sum_i s1_i (1 - a_i) / (1 - a_i / z) (1 - 1/z) = 0,
   L_p = (2 / h sin(p pi / (2 N)))^2, a_i = exp(-k / tau_i), alpha = -ln|z| / k, f = arg z / (2 pi k).
The continuous fit (loss_fit.py) is used as the start; the corners f_i and the amplitudes A_i Q (1/s) are fitted for the four strings at once to the target curve of loss_fit.py
(each string with its own Q: 300 C G, 500 D A).    python -I charts/loss_fit_discrete.py"""
import numpy as np
from scipy.optimize import least_squares
import sys
FMAX2 = float(sys.argv[1]) if len(sys.argv) > 1 else 20000.0
FS = 96000.0
K = 1 / FS
S0 = 1.2
L = 0.69
STR = [("C2", 65.41, 135.0, 8.0e-5, 300.0), ("G2", 98.0, 130.0, None, 300.0), ("D3", 146.83, 140.0, 3.5e-5, 500.0), ("A3", 220.0, 160.0, 2.5e-5, 500.0)]


def geom(f0, T, B_or_EI, is_ei=False):
    rho = T / (2 * L * f0) ** 2
    c2 = T / rho
    EI = B_or_EI if is_ei else B_or_EI * T * L * L / np.pi ** 2
    kap2 = EI / rho
    a = c2 * K * K  # candidate-4 hmin used sigma1 (4 s1 k); the new code uses the sum of the relaxation s1 (set by the caller)
    return rho, c2, kap2


def hmin(c2, kap2, s1sum):
    a = c2 * K * K + 4 * s1sum * K
    return np.sqrt((a + np.sqrt(a * a + 16 * kap2 * K * K)) / 2)


def alpha_discrete(f0, T, EIb, isei, Q, fi, AQ, pmax=400):
    rho, c2, kap2 = geom(f0, T, EIb, isei)
    c = np.sqrt(c2)
    tau = 1 / (2 * np.pi * np.array(fi))
    s1 = (np.array(AQ) / Q) * c2 * tau ** 2
    h0 = hmin(c2, kap2, s1.sum())
    N = int(np.floor(L / h0))
    h = L / N
    a = np.exp(-K / tau)
    out_f, out_a = [], []
    for p in range(1, min(pmax, N // 2) + 1):
        Lp = (2 / h * np.sin(p * np.pi / (2 * N))) ** 2
        w0 = 2 * np.pi * p * f0 * np.sqrt(1 + (np.pi ** 2 * (kap2 * rho / T / L ** 2) * 0 + 0))
        z = np.exp(1j * 2 * np.pi * p * f0 * K)
        for _ in range(60):
            g = sum(s1[i] * (1 - a[i]) / (1 - a[i] / z) for i in range(len(a)))
            F = (1 + S0 * K) * z - 2 + (1 - S0 * K) / z + K * K * (c2 * Lp + kap2 * Lp ** 2) + 2 * K * Lp * g * (1 - 1 / z)
            dg = sum(s1[i] * (1 - a[i]) * (-a[i] / z ** 2) / (1 - a[i] / z) ** 2 * (-1) for i in range(len(a)))
            # numeric derivative
            e = 1e-7
            z2 = z * (1 + e)
            g2 = sum(s1[i] * (1 - a[i]) / (1 - a[i] / z2) for i in range(len(a)))
            F2 = (1 + S0 * K) * z2 - 2 + (1 - S0 * K) / z2 + K * K * (c2 * Lp + kap2 * Lp ** 2) + 2 * K * Lp * g2 * (1 - 1 / z2)
            dz = -F / ((F2 - F) / (z * e))
            z = z + dz
            if abs(dz) < 1e-14:
                break
        out_f.append(np.angle(z) / (2 * np.pi * K))
        out_a.append(-np.log(abs(z)) / K)
    return np.array(out_f), np.array(out_a), N


def target(f, Q):
    old = S0 + (np.pi * 2000.0 / Q - S0) * (f / 2000.0) ** 2
    flat = np.pi * f / Q
    w = np.clip((np.log(f) - np.log(500.0)) / (np.log(1000.0) - np.log(500.0)), 0, 1)
    return np.exp((1 - w) * np.log(old) + w * np.log(np.maximum(flat, S0 * 1.001)))


def resid(p):
    fi = np.exp(p[:2])
    r = []
    for j, (nm, f0, T, b, Q) in enumerate(STR):
        AQ = np.exp(p[2 + 2 * j:4 + 2 * j])
        isei = nm == "G2"
        ff, aa, N = alpha_discrete(f0, T, 3.03e-4 if isei else b, isei, Q, fi, AQ)
        m = (ff > 100) & (ff < 12000)
        wt = np.where(ff[m] < 600.0, 3.0, 1.0)  # the low partials keep their earlier decay: weight 3
        r.append(wt * (np.log(aa[m]) - np.log(target(ff[m], Q))))
    return np.concatenate(r)


p0 = np.concatenate([np.log([2264.0, 12000.0])] + [np.log([7650.4, 83926.8])] * 4)
lo = np.concatenate([np.log([500.0, 3000.0])] + [np.log([100.0, 100.0])] * 4)
hi = np.concatenate([np.log([6000.0, FMAX2])] + [np.log([1e6, 1e6])] * 4)
res = least_squares(resid, np.clip(p0, lo + 1e-9, hi - 1e-9), bounds=(lo, hi), x_scale=0.1)
fi = np.exp(res.x[:2])
print("corners Hz", fi)
print("initial max dB err", np.abs(20 * np.log10(np.exp(resid(p0)))).max(), " fitted", np.abs(20 * np.log10(np.exp(resid(res.x)))).max())
for j, (nm, f0, T, b, Q) in enumerate(STR):
    AQ = np.exp(res.x[2 + 2 * j:4 + 2 * j])
    isei = nm == "G2"
    ff, aa, N = alpha_discrete(f0, T, 3.03e-4 if isei else b, isei, Q, fi, AQ)
    print(nm, "A_i Q 1/s", AQ, "N", N, " Q at", [(int(x), int(np.pi * ff[np.argmin(abs(ff - x))] / aa[np.argmin(abs(ff - x))])) for x in (300, 500, 1000, 2000, 4000, 8000, 10000)])
