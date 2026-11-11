"""Throw-in grab v1 (two fingers taking a card from the hand), from an_tgrab.py on IMG_2952's 8 grabs; peak at 0.2 s.
CLICKS: the main one (0.75-1.9 ms); a softer pre-click 2.6-11 ms before it in 7 of 8 grabs (-12..0 dB, 1.6-2.7 ms);
a partner 1.5-1.8 ms after it in half (-12..-9 dB, about 3 ms). BEDS: the low (150-1.5 kHz) and high (1.5-15 kHz) curves
re the full-band peak, shaped by the dark contact spectrum (peak 300 Hz, -17 dB by 1.2 kHz, a bump at 2.4 kHz)."""
import sys, wave, numpy as np
NAME = sys.argv[1] if len(sys.argv) > 1 else "tgrab1"; rng = np.random.default_rng(int(sys.argv[2]) if len(sys.argv) > 2 else 9)
CK_DB = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0
SR = 48000; P0 = int(0.2 * SR); N = int(0.5 * SR)
cs = np.load("an/cs40.npy"); SP = np.load("an/tgrab_spec.npy")
try: FIT = np.load("an/tgrab_fit.npy")
except OSError: FIT = np.zeros(40)
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
def eq(y, tmpl):
    n = len(y); X = np.fft.rfft(y); f = np.fft.rfftfreq(n, 1 / SR); g = np.interp(np.log(np.maximum(f, 1)), np.log(cs), tmpl); g[f < 120] = -60; g[f > 15000] = -60
    return np.fft.irfft(X * 10 ** (g / 20), n)
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
def click(at, db, width):
    k = int(at * SR); L = int(width * 3 * SR) + 8; t = np.arange(L) / SR
    e = np.minimum(1, t / 0.0002) * np.exp(-t / (width / 2.3)); out = np.zeros(N); out[k:k + L] = rng.standard_normal(L) * e * 10 ** (db / 20); return out
BT = np.array([-0.030, -0.015, -0.008, -0.004, -0.002, 0.0, 0.002, 0.004, 0.008, 0.012, 0.020, 0.030, 0.050, 0.080, 0.130, 0.200])
BL = np.array([-25.9, -22.3, -19.1, -11.2, -4.3, -2.9, -1.7, -2.6, -7.6, -12.7, -16.4, -19.6, -19.9, -22.0, -30, -45])
BH = np.array([-35.7, -34.8, -19.5, -12.0, -7.2, -6.9, -7.9, -15.7, -24.0, -25.3, -26.0, -28.0, -29.0, -34.7, -42, -55])
rms5 = lambda v: np.sqrt(np.convolve(v ** 2, np.ones(240) / 240, "same"))
def grab():
    t0 = P0 / SR; ck = [(0.0, 0.0, rng.uniform(0.00075, 0.0019))]
    if rng.uniform() < 7 / 8: ck.append((-rng.uniform(0.0026, 0.011), rng.uniform(-12, 0), rng.uniform(0.0016, 0.0027)))
    if rng.uniform() < 0.5: ck.append((rng.uniform(0.0015, 0.0018), rng.uniform(-12, -9), rng.uniform(0.0029, 0.0034)))
    con = eq(sum(click(t0 + t, d, w) for t, d, w in ck), SP + rng.uniform(-1.5, 1.5, len(SP)))
    tt = np.arange(N) / SR - t0; ts = rng.uniform(0.85, 1.15)
    bl = np.interp(tt, BT * ts, BL + rng.uniform(-2, 2, len(BL)), left=-90, right=-90); bh = np.interp(tt, BT * ts, BH + rng.uniform(-2, 2, len(BH)), left=-90, right=-90)
    lo = bp(eq(rng.standard_normal(N), np.where(cs < 1500, SP, -60)), 150, 1500); lo = lo / np.sqrt(np.mean(lo ** 2)) * 10 ** (bl / 20)
    hi = bp(eq(rng.standard_normal(N), np.where(cs >= 1500, SP, -60)), 1500, 15000); hi = hi / np.sqrt(np.mean(hi ** 2)) * 10 ** (bh / 20)
    bed = lo + hi; ref = rms5(bed)[P0 - 96:P0 + 96].max()          # the curves are re the full-band 5 ms peak
    con *= ref * 10 ** (CK_DB / 20) / (rms5(con)[P0 - 96:P0 + 96].max() + 1e-12)
    y = eq(bed + con, FIT)
    return y / np.abs(bp(y, 150, 15000)).max() * 10 ** (rng.uniform(-40.2, -25.9) / 20)
for v in range(1, 13): save(f"synth/{NAME}_{v:02d}.wav", grab())
print(NAME, "written")
