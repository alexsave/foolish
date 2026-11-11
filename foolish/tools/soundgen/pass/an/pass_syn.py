"""Pass slide v1 ("shfff"), from pass_an.py on IMG_2936's 6 slides; onset at 0.1 s like parts/pass_k.wav.
BODY: noise in the measured body spectrum (broadband, peak 2.4 kHz) on the measured envelope (a swell to a peak at
~150 ms, held to ~200 ms, -20 dB by ~280 ms), time-stretched 0.9-1.25 per slide (the reps run 230-350 ms); the low band
follows its own curve 15-25 dB under. WEAVE: a faint amplitude flutter at 55-59 and 94-98 Hz (measured 2.3x the mean
of the envelope's modulation spectrum in every slide). SNAGS: 2.5 ms catches, 8-19 dB over the hiss: 1-3 as the fingers
land (0-20 ms), 0-1 in the middle, 2-3 as the card stops (275-345 ms)."""
import sys, os, wave, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))   # tables load from here, so ../sweep can import this
MAIN = __name__ == "__main__"; A = sys.argv if MAIN else []
NAME = A[1] if len(A) > 1 else "pass1"; rng = np.random.default_rng(int(A[2]) if len(A) > 2 else 4)
SNAG_EQ = float(A[4]) if len(A) > 4 else 6.0
AM = float(A[3]) if len(A) > 3 else 0.025
SR = 48000; O = int(0.1 * SR); N = int(0.7 * SR)
cs = np.load(f"{HERE}/../../throwin/an/cs40.npy"); SP = np.load(f"{HERE}/pass_spec.npy")
try: FIT = np.load(f"{HERE}/pass_fit.npy")
except OSError: FIT = np.zeros(40)
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
def eq(y, tmpl):
    n = len(y); X = np.fft.rfft(y); f = np.fft.rfftfreq(n, 1 / SR); g = np.interp(np.log(np.maximum(f, 1)), np.log(cs), tmpl); g[f < 120] = -60; g[f > 15000] = -60
    return np.fft.irfft(X * 10 ** (g / 20), n)
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
T = np.array([-0.02, 0, 20, 40, 60, 80, 100, 120, 140, 160, 180, 200, 220, 240, 260, 280, 300, 330, 360, 400, 450]) / 1000; T[0] = -0.02
EH = np.array([-60, -21.2, -15.5, -12.5, -9.4, -7.3, -6.2, -4.1, -2.3, -1.6, -4.2, -3.9, -6.1, -11.6, -17.3, -21.0, -17.2, -27.6, -30.7, -31.6, -45])
EL = np.array([-60, -26.4, -21.6, -22.9, -21.9, -20.4, -17.2, -20.8, -20.2, -14.6, -18.1, -17.6, -20.4, -22.3, -24.5, -22.6, -18.9, -24.4, -29.0, -27.9, -45])
def snag(n, at, db, width, ref):
    k = int(at * SR); L = int(width * 3 * SR) + 8; t = np.arange(L) / SR; e = np.minimum(1, t / 0.0003) * np.exp(-t / (width / 2.3))
    out = np.zeros(n); out[k:k + L] = rng.standard_normal(L) * e * ref * 10 ** (db / 20); return out
def slide(ts=None, land_knock=True, stop_db=0.0):
    """ts: the time stretch (the card stops ~0.31*ts after onset); None draws the reps' 0.85-1.15. land_knock: the table
    knock as the fingers land on the card. stop_db: added to the stop knock. The defaults are pass7 exactly."""
    if ts is None: ts = rng.uniform(0.85, 1.15)
    tt = np.arange(N) / SR - O / SR
    eh = np.interp(tt, T * ts, EH + np.r_[0, rng.uniform(-1.5, 1.5, len(EH) - 2), 0], left=-90, right=-90)
    el = np.interp(tt, T * ts, EL + np.r_[0, rng.uniform(-1.5, 1.5, len(EL) - 2), 0], left=-90, right=-90)
    hi = bp(eq(rng.standard_normal(N), np.where(cs >= 1000, SP, -60)), 1000, 15000); hi = hi / np.sqrt(np.mean(hi ** 2)) * 10 ** (eh / 20)
    lo = bp(eq(rng.standard_normal(N), np.where(cs < 1000, SP, -60)), 150, 1000); lo = lo / np.sqrt(np.mean(lo ** 2)) * 10 ** (el / 20)
    t = np.arange(N) / SR; f1, f2 = rng.uniform(55, 59), rng.uniform(94, 98)
    am = 1 + AM * (np.sin(2 * np.pi * f1 * t + rng.uniform(0, 6.3)) + 0.9 * np.sin(2 * np.pi * f2 * t + rng.uniform(0, 6.3)))
    body = (hi + lo) * am
    loc = np.sqrt(np.convolve(bp(body, 3000, 15000) ** 2, np.ones(480) / 480, "same"))      # the hiss the snags stand over
    sn = np.zeros(N)
    for _ in range(rng.integers(1, 4)):
        a = rng.uniform(0.002, 0.020); k = O + int(a * ts * SR); sn += snag(N, O / SR + a * ts, rng.uniform(12, 23), rng.uniform(0.0006, 0.0018), loc[k] * 1.6)
    if rng.uniform() < 0.5:
        a = rng.uniform(0.06, 0.22); k = O + int(a * ts * SR); sn += snag(N, O / SR + a * ts, rng.uniform(12, 14), rng.uniform(0.001, 0.002), loc[k] * 1.6)
    for _ in range(rng.integers(2, 4)):
        a = rng.uniform(0.275, 0.345); k = O + int(a * ts * SR); sn += snag(N, O / SR + a * ts, rng.uniform(12, 22), rng.uniform(0.0006, 0.0018), loc[k] * 1.6)
    sn = eq(sn, np.where(cs >= 1500, SP, SP - 12)) * 10 ** (SNAG_EQ / 20)   # the template takes ~SNAG_EQ dB out of them
    # KNOCKS: the table answering the fingers (measured near 200 Hz; here the game's one table, 229 Hz): as they land on the
    # card (-95..+30 ms, -12..+5 dB re the hiss peak), as the card stops (284-345 ms, -10..-3.5 dB), 1-3 weak ones mid-slide
    P = np.sqrt(np.convolve(hi ** 2, np.ones(240) / 240, "same")).max()
    def knock(at, db):
        L = int(0.06 * SR); tk = np.arange(L) / SR; ek = np.minimum(1, tk / 0.001) * np.exp(-tk / rng.uniform(0.006, 0.012))
        z = np.zeros(N); k = int(at * SR); z[k:k + L] = rng.standard_normal(L) * ek; z = bp(eq(z, np.where(cs < 1000, SP, -40)), 150, 1000)
        r = np.exp(-np.pi * 40 / SR); c = 2 * r * np.cos(2 * np.pi * 229 / SR); y = np.zeros(N); y1 = y2 = 0.0
        for i in range(k, min(N, k + L + 4800)): v = (1 - r) * z[i] + c * y1 - r * r * y2; y[i] = v; y2, y1 = y1, v
        z = z + y / (np.abs(y).max() + 1e-12) * np.abs(z).max() * 0.7
        return z / (np.sqrt(np.convolve(z ** 2, np.ones(240) / 240, "same")).max() + 1e-12) * P * 10 ** (db / 20)
    k1 = knock(O / SR + rng.uniform(-0.095, 0.030) * ts, rng.uniform(-12, 5)); k2 = knock(O / SR + rng.uniform(0.284, 0.345) * ts, rng.uniform(-10, -3.5) + stop_db)
    kn = (k1 if land_knock else 0) + k2
    for _ in range(rng.integers(1, 4)): kn += knock(O / SR + rng.uniform(0.11, 0.23) * ts, rng.uniform(-12, -9))
    sn = sn + kn
    y = eq(body + sn, FIT)
    return y / np.abs(bp(y, 150, 15000)).max() * 10 ** (rng.uniform(-36.2, -30.6) / 20)
if MAIN:
    for v in range(1, 13): save(f"synth/{NAME}_{v:02d}.wav", slide())
    print(NAME, "written")
