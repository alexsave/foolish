"""Deck set down v1, from an/deck_an.py on IMG_2950 placements 1-6 (the deck put down on a face-up card).
Writes placements with the thud's peak at 0.2 s, like parts/deck_k.wav.
CONTACTS: the edge or corner touches first: 2-9 bright contacts 40..5 ms before the thud, the brightest at -24..-18 ms
(1 in 6 placements slaps flat instead: the brightest at the thud itself), the others 1-19 dB under it, 0.7-2.8 ms wide,
shaped by the measured contact spectrum; the brightest sits 7.5-17 dB under the thud (3-12 kHz 0.25 ms vs full 5 ms).
THUD: a short low burst (the deck's face meeting the card and cloth) plus the table's modes, measured in every
placement: 196 Hz strongest (+12..+20 dB over the smoothed spectrum, decaying ~140 dB/s), 287 and 397 Hz weaker.
The low band follows the measured curve, the high settle bed its own (about 41 dB under the thud, fading by 250 ms)."""
import sys, wave, numpy as np
NAME = sys.argv[1] if len(sys.argv) > 1 else "deck1"; rng = np.random.default_rng(int(sys.argv[2]) if len(sys.argv) > 2 else 5)
MODE_DB = float(sys.argv[3]) if len(sys.argv) > 3 else -12.0   # Alex: "too much echo" at -6
PRE_LO, PRE_HI = (float(sys.argv[5]), float(sys.argv[6])) if len(sys.argv) > 6 else (-8.0, -4.0)
CLAT = np.load('an/deck_clatter_spec.npy')
CLOW = float(sys.argv[4]) if len(sys.argv) > 4 else 20.0   # contacts' lows: the 1.5 ms analysis windows smear them flat
SR = 48000; P0 = int(0.2 * SR); N = int(0.8 * SR)
cs = np.load("an/cs40.npy"); CSP = np.load("an/deck_contact_spec.npy"); BSP = np.load("an/deck_bed_spec.npy")
# the bed spectrum already holds the table's modes; smooth them out of it below 500 Hz (5 bands) so the resonators are not counted twice
_sm = np.convolve(np.pad(BSP, 2, mode='edge'), np.ones(5) / 5, 'valid'); BSP = np.where(cs < 500, _sm, BSP)
try: FIT = np.load("an/deck_fit.npy")
except OSError: FIT = np.zeros(40)
# ONE TABLE for the whole game (Alex, 2026-11-02: "ring at 229hz", the throw-in table). This recording's table rang at
# 196 Hz; its modes keep their ratios and move up by 229/196, and the fit's low part (shaped around them) moves with them.
TABLE = 229.0 / 196.0
FIT = np.where(cs < 600, np.interp(np.log(cs / TABLE), np.log(cs), FIT), FIT)
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
def eq(y, tmpl):
    n = len(y); X = np.fft.rfft(y); f = np.fft.rfftfreq(n, 1 / SR); g = np.interp(np.log(np.maximum(f, 1)), np.log(cs), tmpl); g[f < 90] = -60; g[f > 15000] = -60
    return np.fft.irfft(X * 10 ** (g / 20), n)
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
def reson(x, freq, bw):
    r = np.exp(-np.pi * bw / SR); c = 2 * r * np.cos(2 * np.pi * freq / SR); y = np.zeros_like(x); y1 = y2 = 0.0
    for i in range(len(x)): v = (1 - r) * x[i] + c * y1 - r * r * y2; y[i] = v; y2, y1 = y1, v
    return y
def click(n, at, db, width):
    k = int(at * SR); L = int(width * 3 * SR) + 8; t = np.arange(L) / SR
    e = np.minimum(1, t / 0.0001) * np.exp(-t / (width / 2.3)); out = np.zeros(n); seg = rng.standard_normal(L) * e * 10 ** (db / 20)
    lo, hi = max(0, k), min(n, k + L); out[lo:hi] = seg[lo - k: hi - k]; return out
BT = np.array([-0.080, -0.040, -0.020, -0.008, -0.003, 0.0, 0.003, 0.008, 0.015, 0.025, 0.040, 0.060, 0.090, 0.130, 0.180, 0.250, 0.350])
BLv = np.array([-39, -32, -30, -30, -20, 0, -2.4, -9, -14.3, -21.5, -20.8, -24.9, -20.8, -30.4, -34.6, -43.8, -50.6])   # 2 dB under the measured tail; the echo was the ring   # the tail 4 dB under the measured curve: the modes carry the rest   # 100-1k re the thud
BHv = np.array([-57, -52, -33, -39, -44, -41, -44, -42, -44, -47, -51, -56, -63, -68, -72, -76, -82])             # 2-12k re the thud
rms5 = lambda v: np.sqrt(np.convolve(v ** 2, np.ones(240) / 240, "same"))
def deck():
    t0 = P0 / SR; ts = rng.uniform(0.85, 1.15); tt = np.arange(N) / SR - t0
    # thud: low noise on the measured low-band curve (dB re its own peak), shaped by the bed spectrum below 1.5 kHz
    lo = eq(rng.standard_normal(N), np.where(cs < 1500, BSP, -60)); lo = bp(lo, 90, 1500)
    bl = np.interp(tt, BT * ts, BLv + rng.uniform(-2, 2, len(BLv)), left=-90, right=-90); lo = lo / np.sqrt(np.mean(lo ** 2)) * 10 ** (bl / 20)
    # the table's modes, excited by the thud itself, set over their neighbours as measured
    for fq, ex, bw in (((196.0 if rng.uniform() < 5 / 6 else 211.0) * TABLE, rng.uniform(12, 20), 9.0), (287.0 * TABLE, rng.uniform(10, 13), 11.0), (397.0 * TABLE, rng.uniform(9, 18), 13.0)):
        m = reson(lo, fq, bw); w = slice(P0, P0 + 9600)
        nb = np.sqrt(np.mean((bp(lo, fq - 40, fq - 15) + bp(lo, fq + 15, fq + 40))[w] ** 2) / 2)
        lo = lo + m / (np.sqrt(np.mean(bp(m, fq - 8, fq + 8)[w] ** 2)) + 1e-12) * nb * 10 ** ((ex - 8 + MODE_DB) / 20)   # -8: a 16 Hz-wide band vs one FFT bin's excess
    thud = rms5(lo).max()
    # the first touch: the deck's corner lands 8-14 ms before it goes flat, a smaller low knock (3 ms decay), then a dip
    Lp = int(0.03 * SR); tp_ = np.arange(Lp) / SR; ep = np.minimum(1, tp_ / 0.001) * np.exp(-tp_ / 0.003)
    pre = np.zeros(N); kp = P0 - int(rng.uniform(0.010, 0.016) * SR); pre[kp:kp + Lp] = rng.standard_normal(Lp) * ep
    pre = bp(eq(pre, np.where(cs < 1500, BSP, -60)), 90, 1500); lo = lo + pre / (rms5(pre).max() + 1e-12) * thud * 10 ** (rng.uniform(PRE_LO, PRE_HI) / 20)
    hi = eq(rng.standard_normal(N), np.where(cs >= 1500, BSP, -60)); hi = bp(hi, 1500, 12000)
    bh = np.interp(tt, BT * ts, BHv + rng.uniform(-2, 2, len(BHv)), left=-90, right=-90); hi = hi / np.sqrt(np.mean(hi ** 2)) * 10 ** (bh / 20) * thud
    # contacts
    slap = rng.uniform() < 1 / 6; ck = [(0.0 if slap else -rng.uniform(0.018, 0.024), 0.0, rng.uniform(0.0007, 0.0025))]
    for _ in range(rng.integers(1, 9)): ck.append((-rng.uniform(0.005, 0.040), rng.uniform(-19, -1), rng.uniform(0.0007, 0.0028)))
    if slap: ck += [(rng.uniform(0.001, 0.003), rng.uniform(-19, -13), 0.002), (rng.uniform(0.010, 0.012), rng.uniform(-28, -20), 0.002)]
    # the edge clatter: the deck's edges hitting in a dense burst ~20 ms before it lands flat (5 ms rise, 5 ms decay),
    # its 1-15 kHz peak 14-20 dB under the thud's low peak; a slap puts it at the thud instead, at about the thud's level
    Lc = int(0.04 * SR); tc = np.arange(Lc) / SR; ec = np.minimum(1, tc / 0.005) * np.exp(-np.maximum(tc - 0.005, 0) / 0.005)
    cc = (t0 - 0.003) if slap else (t0 + ck[0][0] - 0.005)   # centred on the brightest contact
    cl = np.zeros(N); kc = int(cc * SR); cl[kc:kc + Lc] = rng.standard_normal(Lc) * ec
    cl = bp(eq(cl, np.where(cs >= 1000, CLAT, -40)), 1000, 15000)
    cl *= thud * 10 ** ((rng.uniform(-3, 1) if slap else rng.uniform(-20, -14)) / 20) / (rms5(cl).max() + 1e-12)
    con = cl + eq(sum(click(N, t0 + t, d, w) for t, d, w in ck), CSP - CLOW * np.clip(np.log2(1500 / cs), 0, 1) - np.where((cs > 1000) & (cs < 4000), 5, 0) + rng.uniform(-1.5, 1.5, len(CSP)))
    top = np.sqrt(np.convolve(bp(con - cl, 3000, 12000) ** 2, np.ones(12) / 12, "same")).max()
    con = cl + (con - cl) * thud * 10 ** ((rng.uniform(5, 10) if slap else rng.uniform(-15, -7)) / 20) / (top + 1e-12)
    y = eq(lo + hi + con, FIT)
    return y / np.abs(bp(y, 100, 15000)).max() * 10 ** (rng.uniform(-25.7, -4.9) / 20)
for v in range(1, 13): save(f"synth/{NAME}_{v:02d}.wav", deck())
print(NAME, "written")
