"""Cover landing v1 (a card landing ON a card), from cov_an.py on Alex's cover reps 1 2 3 5 8 10 12 13 14 (IMG_2934);
built on the throw-in synth (../an/throw_syn.py) with every table re-measured. Kept below: the throw-in's notes, from an/throw_an.py on reps 2-8 of IMG_2933 (a single card thrown onto cloth: the leading edge, then
the slap flat). STRIKES: the main one (1-2.5 ms), 1-2 edge strikes 1.5-14 ms before it (-16..-4 dB), half the time a
partner 1-3 ms after (-10..-5 dB), a third of the time a bounce at +13..+35 ms (-25..-19 dB); shaped by the measured
strike spectrum. BED: the low body (it outweighs the strikes: +4 dB at +6 ms) and the highs, on their measured curves,
in dB re the loudest strike's 3-15 kHz 0.25 ms level."""
import sys, wave, numpy as np
rng = np.random.default_rng(int(sys.argv[2]) if len(sys.argv) > 2 else 7); SR = 48000; NAME = sys.argv[1] if len(sys.argv) > 1 else "cov1"
cs = np.load("../an/cs40.npy"); SS = np.load("cov_strike_spec.npy"); BS = np.load("cov_bed_spec.npy")
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
def eq(y, tmpl):
    n = len(y); X = np.fft.rfft(y); f = np.fft.rfftfreq(n, 1 / SR); g = np.interp(np.log(np.maximum(f, 1)), np.log(cs), tmpl); g[f < 120] = -60; g[f > 15000] = -60
    return np.fft.irfft(X * 10 ** (g / 20), n)
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
BT = np.array([-20, -8, -3, 0, 3, 6, 10, 15, 20, 30, 40, 60, 80, 100, 130, 170, 220, 280]) / 1000
BL = np.array([-34.5, -29.5, -18, -14, -14.2, -10.3, -16.3, -18.9, -19.0, -21.8, -29.7, -30.9, -30.3, -34.0, -42.0, -40.2, -46.6, -51.4])   # measured, less 4 dB after the hit (the synth's tail ran 4-5 dB long)
BH = np.array([-36.3, -21.1, -15.0, -20.6, -25.5, -24.2, -31.8, -35.1, -37.0, -39.1, -41.4, -41.0, -43.8, -47.5, -50.2, -53.8, -58.0, -60])
TOP = -np.clip((np.log2(cs / 7000)) * 18, 0, 30)   # the real throw-ins are ~18 dB/oct darker above 7 kHz than the 1.5 ms strike windows
SLAP = float(sys.argv[3]) if len(sys.argv) > 3 else -6
MODE = 3.3
LOWPK = float(sys.argv[7]) if len(sys.argv) > 7 else 0
RISE = float(sys.argv[6]) if len(sys.argv) > 6 else 0.0001
STILT = float(sys.argv[4]) if len(sys.argv) > 4 else 0
SLTILT = float(sys.argv[5]) if len(sys.argv) > 5 else 10
try: FIT = np.load('cov_fit.npy')
except OSError: FIT = np.zeros(40)
def strike(n, at, db, width):
    k = int(at * SR); L = int(width * 3 * SR) + 8; t = np.arange(L) / SR
    env = np.minimum(1, t / RISE) * np.exp(-t / (width / 2.3))
    out = np.zeros(n); seg = rng.standard_normal(L) * env * 10 ** (db / 20); lo, hi = max(0, k), min(n, k + L); out[lo:hi] = seg[lo - k: hi - k]; return out
def throw():
    n = int(0.40 * SR); t0 = 0.06
    # strikes as measured on the 9 covers: the main one; a partner 0.9-3 ms after (5 of 9); a pre-strike 2-8.5 ms before
    # (4 of 9); a bounce 5.6-13 ms after (4 of 9)
    ck = [(0.0, 0.0, rng.uniform(0.0012, 0.0025))]
    if rng.uniform() < 5 / 9: ck.append((rng.uniform(0.0009, 0.003), rng.uniform(-11, -5), rng.uniform(0.0022, 0.0032)))
    if rng.uniform() < 4 / 9: ck.append((-rng.uniform(0.002, 0.0085), rng.uniform(-11, -1), rng.uniform(0.0015, 0.003)))
    if rng.uniform() < 4 / 9: ck.append((rng.uniform(0.0056, 0.013), rng.uniform(-25, -3), rng.uniform(0.0013, 0.0028)))
    st = eq(sum(strike(n, t0 + t, d, w) for t, d, w in ck), SS + TOP - STILT * np.clip(np.log2(cs / 1000), 0, None) + rng.uniform(-1.5, 1.5, len(SS)))
    # the slap: the card's face meeting the cloth is a short noise burst, not a click (1-4 kHz kurtosis ~6, not ~13)
    L = int(0.03 * SR); tq = np.arange(L) / SR; env = np.minimum(1, tq / 0.0008) * np.exp(-tq / rng.uniform(0.003, 0.005))
    sl = np.zeros(n); k0 = int(t0 * SR) - 24; sl[k0:k0 + L] = rng.standard_normal(L) * env
    sl = eq(sl, SS + TOP - SLTILT * np.clip(np.log2(cs / 700), 0, None)); sl *= np.abs(st).max() / (np.abs(sl).max() + 1e-12) * 10 ** (SLAP / 20); st = st + sl
    hs = bp(st, 3000, 15000); ref = np.sqrt(np.convolve(hs ** 2, np.ones(12) / 12, "same")).max()
    tt = np.arange(n) / SR - t0; ts = rng.uniform(0.85, 1.15)
    bl = np.interp(tt, BT * ts, BL + LOWPK * np.isin(BT, [-0.003, 0, 0.003, 0.006, 0.010]) + rng.uniform(-1.5, 1.5, len(BL)), left=-80, right=-80); bh = np.interp(tt, BT * ts, BH + rng.uniform(-1.5, 1.5, len(BH)), left=-80, right=-80)
    lo_t = np.where(cs < 1500, BS, -60); hi_t = np.where(cs >= 1500, BS, -60)
    b1 = bp(eq(rng.standard_normal(n), lo_t), 150, 1500); b1 = b1 / np.sqrt(np.mean(b1 ** 2)) * 10 ** (bl / 20) * ref
    b2 = bp(eq(rng.standard_normal(n), hi_t), 1500, 15000); b2 = b2 / np.sqrt(np.mean(b2 ** 2)) * 10 ** (bh / 20) * ref
    y = st + b1 + b2
    # the table's body mode: 222-238 Hz, ~6 Hz wide, measured 3-6 dB over its neighbours from +10 to +90 ms
    fm = rng.uniform(226, 234); r = np.exp(-np.pi * 6.0 / SR); a1, a2 = 2 * r * np.cos(2 * np.pi * fm / SR), -r * r
    ex = bp(y, 150, 15000); md = np.zeros(n); y1 = y2 = 0.0
    for i in range(n): v = (1 - r) * ex[i] + a1 * y1 + a2 * y2; md[i] = v; y2, y1 = y1, v
    w = slice(int((t0 + 0.01) * SR), int((t0 + 0.09) * SR)); nb = np.sqrt(np.mean((bp(y, 170, 205) + bp(y, 255, 290))[w] ** 2) / (70 / 30))
    y = y + md / (np.sqrt(np.mean(bp(md, 215, 245)[w] ** 2)) + 1e-12) * nb * 10 ** (MODE / 20)
    y = eq(y, FIT)
    return y / np.abs(bp(y, 150, 15000)).max() * 10 ** (rng.uniform(-16.6, -1.5) / 20)
for v in range(1, 13): save(f"synth/{NAME}_{v:02d}.wav", throw())
print(NAME, "written")
