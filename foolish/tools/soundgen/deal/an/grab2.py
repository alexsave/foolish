"""Grab v2 (prototype) from the click-by-click measurement of grabs 2-5 (an/clicks.py): a CLICK TRACK (3-7 approach
clicks -40..-4 ms at -26..-12 dB, the main click with a partner 1-7 ms away at -9..-1.5 dB, 0-2 late clicks +15..+30 ms
at -25..-3 dB; each 0.7-3 ms, the main 0.7-1 ms; shaped by the click spectrum) over a BED TRACK (low and quiet, shaped
by the bed spectrum, -18..-31 dB under the main click in the lows, its level over time from the measurement)."""
import wave, numpy as np
rng = np.random.default_rng(23); SR = 48000
cs = np.load("an/cs40.npy"); CS = np.load("an/click_spec.npy"); BS = np.load("an/bed_spec.npy")
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
def eq(y, tmpl):
    n = len(y); X = np.fft.rfft(y); f = np.fft.rfftfreq(n, 1 / SR); g = np.interp(np.log(np.maximum(f, 1)), np.log(cs), tmpl); g[f < 120] = -60; g[f > 15000] = -60
    return np.fft.irfft(X * 10 ** (g / 20), n)
BT = np.array([-0.045, -0.008, -0.003, 0.003, 0.008, 0.015, 0.025, 0.040, 0.060, 0.080, 0.100, 0.130, 0.170])
BL = np.array([-32, -27.4, -23.9, -19.1, -17.8, -20.2, -29.0, -28.4, -26.1, -29.5, -31.4, -34, -40])
BHT = np.array([-0.045, -0.008, -0.003, 0.005, 0.010, 0.020, 0.030, 0.040, 0.060, 0.080, 0.100, 0.130, 0.160])
BH = np.array([-40, -36, -30, -18, -22, -24, -27, -29, -34, -39, -44, -49, -55])
def click(n, at, db, width):
    """a short burst: noise under a 0.05 ms rise and an exponential fall lasting ~width to -10 dB"""
    k = int(at * SR); L = int(width * 2.5 * SR) + 8; tau = width / 2.3
    t = np.arange(L) / SR; env = np.minimum(1, t / 0.00005) * np.exp(-t / tau)
    out = np.zeros(n); seg = rng.standard_normal(L) * env * 10 ** (db / 20)
    lo, hi = max(0, k), min(n, k + L); out[lo:hi] = seg[lo - k: hi - k]; return out
def grab():
    pre = 0.05; n = int(0.22 * SR); t0 = pre
    ck = [(0.0, 0.0, rng.uniform(0.0007, 0.001))]
    for _ in range(rng.integers(3, 8)): ck.append((rng.uniform(-0.040, -0.004), rng.uniform(-26, -12), rng.uniform(0.0012, 0.003)))
    ck.append((rng.choice([-1, 1]) * rng.uniform(0.001, 0.007), rng.uniform(-9, -1.5), rng.uniform(0.0008, 0.0025)))
    for _ in range(rng.integers(0, 3)): ck.append((rng.uniform(0.015, 0.030), rng.uniform(-25, -3), rng.uniform(0.0012, 0.003)))
    clicks = sum(click(n, t0 + t, d, w) for t, d, w in ck)
    clicks = eq(clicks, CS)
    peak_click = np.abs(clicks[int(t0 * SR) - 48: int(t0 * SR) + 96]).max()
    # the bed in two bands: the lows on the low curve, the highs dying faster and steadily (an/tailcmp.py: real highs
    # -11 dB at 20 ms ... -32 at 100 ms; one shared envelope had left the highs on a plateau, a run-on hiss)
    tt = np.arange(n) / SR - t0; ts = rng.uniform(0.85, 1.15)
    bl = np.interp(tt, BT * ts, BL + rng.uniform(-2, 2, len(BL)), left=-80, right=-80)
    bh = np.interp(tt, BHT * ts, BH + rng.uniform(-1.5, 1.5, len(BH)), left=-80, right=-80)
    lo_t = np.where(cs < 1500, BS, -60); hi_t = np.where(cs >= 1500, BS, -60)
    bed_lo = eq(rng.standard_normal(n), lo_t); bed_lo = bed_lo / (np.sqrt(np.mean(bed_lo ** 2)) + 1e-12) * 10 ** (bl / 20) * peak_click * 0.5
    bed_hi = eq(rng.standard_normal(n), hi_t); bed_hi = bed_hi / (np.sqrt(np.mean(bed_hi ** 2)) + 1e-12) * 10 ** (bh / 20) * peak_click * 0.5
    bed = bed_lo + bed_hi
    y = clicks + bed
    return y / np.abs(y).max() * 10 ** (rng.uniform(-37, -24) / 20)
for v in range(1, 13): save(f"synth/grab2_{v:02d}.wav", grab())
print("grab2 written")
