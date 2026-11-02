"""Landing v1 (prototype), modelled on landing 4 of IMG_2942 (Alex: the best one), varied within the ranges of
landings 1, 2, 3 and 5. CONTACTS: a double main contact (two ~1 ms apart, the first 0-2 dB down), a follow-up at
+5..+15 ms (-8..-4 dB), a pre-contact at -8..-25 ms (-20..-9 dB, 70% of landings); each 2-3 ms wide (felt), shaped
by landing 4's dark contact spectrum. BED: lows and highs on landing 4's own decay curves (the highs die faster)."""
import wave, numpy as np
rng = np.random.default_rng(31); SR = 48000
cs = np.load("an/cs40.npy"); CS = np.load("an/land4_click_spec.npy"); BS = np.load("an/land4_bed_spec.npy")
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
def eq(y, tmpl):
    n = len(y); X = np.fft.rfft(y); f = np.fft.rfftfreq(n, 1 / SR); g = np.interp(np.log(np.maximum(f, 1)), np.log(cs), tmpl); g[f < 120] = -60; g[f > 15000] = -60
    return np.fft.irfft(X * 10 ** (g / 20), n)
# landing 4's bed curves (dB re contact), from an/land_clicks.py: the lows and the faster highs
LT = np.array([-0.012, -0.008, -0.003, 0.003, 0.006, 0.010, 0.015, 0.020, 0.030, 0.040, 0.060, 0.080, 0.100, 0.130, 0.170])
LL = np.array([-30, -16, -14, -6, -10, -12, -18.4, -15.3, -16.0, -17.6, -21.9, -23.1, -22, -21, -27])
LH = np.array([-34, -24, -18, -13, -19, -23, -25, -28, -30, -32, -36, -38, -40, -44, -50])
def contact(n, at, db, width):
    k = int(at * SR); L = int(width * 2.5 * SR) + 8; tau = width / 2.3
    t = np.arange(L) / SR; env = np.minimum(1, t / 0.0003) * np.exp(-t / tau)      # a softer onset than a grab's click: felt
    out = np.zeros(n); seg = rng.standard_normal(L) * env * 10 ** (db / 20); lo, hi = max(0, k), min(n, k + L); out[lo:hi] = seg[lo - k: hi - k]; return out
def land():
    n = int(0.26 * SR); t0 = 0.05
    ck = [(0.0, 0.0, rng.uniform(0.002, 0.003)), (rng.uniform(-0.0012, -0.0006), rng.uniform(-2, 0), rng.uniform(0.002, 0.003)),
          (rng.uniform(0.005, 0.015), rng.uniform(-8, -4), rng.uniform(0.0015, 0.0025))]
    if rng.uniform() < 0.7: ck.append((rng.uniform(-0.025, -0.008), rng.uniform(-20, -9), rng.uniform(0.0015, 0.0025)))
    con = eq(sum(contact(n, t0 + t, d, w) for t, d, w in ck), CS + np.where(cs < 320, 2.5, 0) + rng.uniform(-2, 2, len(CS)))   # +2.5 dB under 320 Hz: it measured 3 dB short at 250
    pc = np.abs(con[int(t0 * SR) - 96: int(t0 * SR) + 144]).max()
    tt = np.arange(n) / SR - t0; ts = rng.uniform(0.85, 1.15)
    bl = np.interp(tt, LT * ts, LL + rng.uniform(-2, 2, len(LL)), left=-80, right=-80); bh = np.interp(tt, LT * ts, LH + rng.uniform(-2, 2, len(LH)), left=-80, right=-80)
    lo_t = np.where(cs < 1500, BS, -60); hi_t = np.where(cs >= 1500, BS, -60)
    b1 = eq(rng.standard_normal(n), lo_t); b1 = b1 / (np.sqrt(np.mean(b1 ** 2)) + 1e-12) * 10 ** (bl / 20) * pc * 0.5
    b2 = eq(rng.standard_normal(n), hi_t); b2 = b2 / (np.sqrt(np.mean(b2 ** 2)) + 1e-12) * 10 ** (bh / 20) * pc * 0.5
    y = con + b1 + b2
    return y / np.abs(y).max() * 10 ** (rng.uniform(-40, -35) / 20)
for v in range(1, 13): save(f"synth/land1_{v:02d}.wav", land())
print("land1 written")
