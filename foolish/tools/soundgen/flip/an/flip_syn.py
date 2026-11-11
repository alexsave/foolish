"""Flip v1, from an/flip_an.py and an/flip_spec.py on IMG_2951 (the thumb pushes the top card off sideways, the other
hand takes it and turns it over). Writes full flips with the snap at 0.35 s (like parts/flip_k.wav), plus the same
slide-only and whup-only cuts the analysis made, so measure.py can compare each part.
SLIDE: noise shaped by the measured slide spectrum, through a resonance that FALLS 3.6 -> 1.5 kHz (LPC on reps
1, 3, 4, 5), on the measured onset->snap envelope, 54-118 ms long, peaking -4..-22 dB re the snap.
SNAP: 4 of 7 reps have one (1.3-2.9 ms; sometimes a partner at +1.6 ms, -8 dB, or a late tick at +16 ms, -16 dB);
the other 3 reps release softly and are 10-15 dB quieter overall.
WHUPS: 1-4 broad low pulses (about 320 Hz wide, -10 dB width ~18 ms), the first at +19..+58 ms, then every 40-55 ms,
each lower than the last (565 -> 372 -> 287 Hz), the later ones 5-15 dB down: the card flapping as it turns.
BEDS: low and high after the snap on their measured curves."""
import sys, wave, numpy as np
NAME = sys.argv[1] if len(sys.argv) > 1 else "flip1"; rng = np.random.default_rng(int(sys.argv[2]) if len(sys.argv) > 2 else 3)
SR = 48000; S0 = int(0.35 * SR); N = int(0.9 * SR)
cs = np.load("an/cs40.npy"); SLS = np.load("an/flip_slide_spec.npy"); SNS = np.load("an/flip_snap_spec.npy"); WHS = np.load("an/flip_whup_spec.npy")
WH_DB = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0   # the snap came down 5 dB; the whups did not      # trims, for sweeps
REL_DB = float(sys.argv[5]) if len(sys.argv) > 5 else -24
SNAP_BODY = float(sys.argv[6]) if len(sys.argv) > 6 else 12.0   # Alex: "too much clapping" at -6
RES_MIX = float(sys.argv[4]) if len(sys.argv) > 4 else 0.15
try: SFIT = np.load("an/flip_slide_fit.npy")
except OSError: SFIT = np.zeros(40)
try: FIT = np.load('an/flip_fit.npy')
except OSError: FIT = np.zeros(40)
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
def eq(y, tmpl):
    n = len(y); X = np.fft.rfft(y); f = np.fft.rfftfreq(n, 1 / SR); g = np.interp(np.log(np.maximum(f, 1)), np.log(cs), tmpl); g[f < 100] = -60; g[f > 15000] = -60
    return np.fft.irfft(X * 10 ** (g / 20), n)
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
def reson(x, freq, bw):
    """two-pole resonator, freq may be an array (per sample); unity-ish peak gain"""
    freq = np.broadcast_to(freq, x.shape); r = np.exp(-np.pi * bw / SR); y = np.zeros_like(x); y1 = y2 = 0.0
    for i in range(len(x)):
        v = (1 - r) * x[i] + 2 * r * np.cos(2 * np.pi * freq[i] / SR) * y1 - r * r * y2; y[i] = v; y2, y1 = y1, v
    return y
SLE = np.array([-20.9, -17.8, -15.2, -9.1, -6.2, -5.3, -2.3, -4.6, -4.4, -3.6, 0.0])   # onset -> snap, re slide peak (last point = at the snap, the slide's own level)
BT = np.array([0.005, 0.020, 0.040, 0.060, 0.090, 0.130, 0.180, 0.250, 0.350, 0.450, 0.540])
BLv = np.array([-28, -35, -34, -37, -36, -37, -44, -54, -62, -70, -80])   # the tail keeps falling (it used to stop dead at 250 ms)                                 # low bed re snap, less the whups' share
BHv = np.array([-13.5, -25.9, -31.4, -35.2, -38.4, -45.2, -48.5, -57.9, -64, -71, -80])
def click(n, at, db, width):
    k = int(at * SR); L = int(width * 3 * SR) + 8; t = np.arange(L) / SR
    e = np.minimum(1, t / 0.0001) * np.exp(-t / (width / 2.3)); out = np.zeros(n); seg = rng.standard_normal(L) * e * 10 ** (db / 20)
    lo, hi = max(0, k), min(n, k + L); out[lo:hi] = seg[lo - k: hi - k]; return out
def lvl_snap(y):   # the analysis' reference: 3-12 kHz, 0.25 ms RMS, max within +-2 ms of the snap
    h = np.sqrt(np.convolve(bp(y, 3000, 12000) ** 2, np.ones(12) / 12, "same")); return h[S0 - 96: S0 + 96].max()
def flip():
    hard = rng.uniform() < 4 / 7; t0 = S0 / SR
    # snap
    if hard:
        ck = [(0.0, 0.0, rng.uniform(0.0025, 0.0040))]
        if rng.uniform() < 0.25: ck.append((0.0016, -8.0, rng.uniform(0.002, 0.003)))
        if rng.uniform() < 0.25: ck.append((rng.uniform(0.012, 0.018), -16.0, 0.002))
        snap = eq(sum(click(N, t0 + t, d, w) for t, d, w in ck), SNS - SNAP_BODY * np.clip(np.log2(1500 / cs), 0, 1) + rng.uniform(-1.5, 1.5, len(SNS)))   # Alex: "slapping" = the snap's low-mid body
        # the release: the card's edge leaving the fingers, a 0.5 ms rise and 2-4 ms decay of dark noise under the click
        Lr = int(0.02 * SR); tr = np.arange(Lr) / SR; er = np.minimum(1, tr / 0.0005) * np.exp(-tr / rng.uniform(0.002, 0.004))
        rel = np.zeros(N); rel[S0 - 24: S0 - 24 + Lr] = rng.standard_normal(Lr) * er; rel = eq(rel, SNS - 8 * np.clip(np.log2(cs / 1500), 0, None))
        snap = snap + rel / (np.abs(rel).max() + 1e-12) * np.abs(snap).max() * 10 ** (REL_DB / 20)
    else:   # a soft release: a dull 4-6 ms brush instead of a click, measured ~10 dB under a hard snap's band level
        snap = eq(click(N, t0 - 0.002, 0.0, rng.uniform(0.004, 0.006)), SNS - np.where(cs > 2500, 6, 0))
    ref = lvl_snap(snap)
    # slide, ending at the snap
    L = rng.uniform(0.054, 0.118); a = int((t0 - L) * SR); n = S0 - a + int(0.004 * SR)
    noise = eq(rng.standard_normal(n), SLS + SFIT + rng.uniform(-1.5, 1.5, len(SLS)))
    fr = np.interp(np.arange(n) / n, [0, 0.25, 0.5, 0.75, 1], [rng.uniform(3300, 3800), rng.uniform(2000, 2500), rng.uniform(1400, 1700), rng.uniform(1450, 1700), rng.uniform(1500, 1800)])
    res = reson(noise, fr, rng.uniform(250, 450)); res *= np.sqrt(np.mean(noise ** 2) / (np.mean(res ** 2) + 1e-20))
    body = (1 - RES_MIX) * noise + RES_MIX * res
    # rise to a peak 9-47 ms before the snap, then the measured fall (-1.5 dB at +5 ms ... -16.4 at +30 ms, ~0.55 dB/ms on)
    tp = int(rng.uniform(0.009, min(0.047, L * 0.45)) * SR); kp = (S0 - a) - tp
    rise = np.interp(np.arange(kp) / kp, np.linspace(0, 1, 9), np.array([-21, -18, -16, -12, -8.5, -6, -4, -2, 0]) + rng.uniform(-1.5, 1.5, 9))
    rise[: int(0.015 * SR)] += np.linspace(-25, 0, len(rise[: int(0.015 * SR)]))   # eased in from silence over 15 ms, not switched on at -21 dB
    dt = np.arange(n - kp) / SR * 1000; fall = np.interp(dt, [0, 5, 10, 20, 30, 60], [0, -1.5, -4.5, -12, -16.4, -33])
    e = np.concatenate([rise, fall])
    body *= 10 ** (e / 20); hb = np.sqrt(np.convolve(bp(body, 2000, 12000) ** 2, np.ones(240) / 240, "same")).max()
    slide = np.zeros(N); slide[a:a + n] = body / (hb + 1e-12) * ref * 10 ** ((rng.uniform(-23, -12) if hard else rng.uniform(-6, -3)) / 20)
    # whups
    kcount = rng.choice([1, 2, 3, 4], p=[3 / 7, 1 / 7, 2 / 7, 1 / 7]); t = rng.uniform(0.019, 0.058) if kcount == 1 else rng.uniform(0.019, 0.035)
    f = rng.uniform(420, 570) if rng.uniform() < 0.7 else rng.uniform(220, 340); wh = np.zeros(N)
    for i in range(kcount):
        db = rng.uniform(-13, -1) if i == 0 else rng.uniform(-27, -7)
        Lw = int(0.06 * SR); tw = np.arange(Lw) / SR; ew = np.minimum(1, tw / 0.004) * np.exp(-np.maximum(tw - 0.004, 0) / rng.uniform(0.005, 0.008))
        p = reson(rng.standard_normal(Lw), f, 320.0) * ew; p = eq(p, WHS * 0.3)                 # the template only tilts it a little
        lo_ = bp(np.pad(p, (480, 480)), 150, 800); pk = np.sqrt(np.convolve(lo_ ** 2, np.ones(240) / 240, "same")).max()
        k = S0 + int(t * SR) - int(0.004 * SR); seg = p / (pk + 1e-12) * ref * 10 ** ((db + WH_DB) / 20); wh[k:k + Lw] += seg[: N - k]
        t += rng.uniform(0.040, 0.055); f *= rng.uniform(0.62, 0.72) if i == 0 else rng.uniform(0.75, 0.85)
    # beds after the snap
    tt = np.arange(N) / SR - t0; ts = rng.uniform(0.85, 1.15)
    bl = np.interp(tt, BT * ts, BLv + rng.uniform(-2, 2, len(BLv)), left=-90, right=-90); bh = np.interp(tt, BT * ts, BHv + rng.uniform(-2, 2, len(BHv)), left=-90, right=-90)
    b1 = bp(rng.standard_normal(N), 150, 1500); b1 *= 10 ** (bl / 20) * ref / np.sqrt(np.mean(b1 ** 2))
    b2 = eq(rng.standard_normal(N), np.where(cs > 1500, SLS, -60)); b2 = bp(b2, 1500, 12000); b2 *= 10 ** (bh / 20) * ref / np.sqrt(np.mean(b2 ** 2))
    # levels as measured: the slide's own peak is steady at -25..-20 dBFS in every rep; a hard snap puts the whole
    # 12-19.5 dB above it, a soft release leaves the slide the loudest thing (the rest 1-4 dB under it)
    rest = eq(snap + wh + b1 + b2, FIT); sl = eq(slide, FIT)
    pk = lambda v: np.abs(bp(v, 100, 15000)).max() + 1e-12
    Ls = 10 ** (rng.uniform(-25, -20) / 20); sl *= Ls / pk(sl)
    rest *= Ls * 10 ** ((rng.uniform(0, 6) if hard else rng.uniform(-4, -1)) / 20) / pk(rest)
    return sl + rest
fade = lambda n: np.minimum(1, np.minimum(np.arange(n), n - 1 - np.arange(n)) / 96)
for v in range(1, 13):
    y = flip(); save(f"synth/{NAME}_{v:02d}.wav", y)
    w = y[S0 + 240: S0 + int(0.25 * SR)]; save(f"synth/{NAME}w_{v:02d}.wav", w * fade(len(w)))
    # the slide cut exactly as flip_an.py makes it
    hi = 20 * np.log10(np.sqrt(np.convolve(bp(y, 2000, 12000) ** 2, np.ones(240) / 240, "same")) + 1e-12)[::48]; s0 = S0 // 48
    sp = int(np.argmax(hi[s0 - 250: s0 - 3])) + s0 - 250; on = next(i for i in range(sp, 0, -1) if hi[i] < hi[sp] - 20)
    c = y[on * 48 - 2880: S0 - 96]; save(f"synth/{NAME}s_{v:02d}.wav", c * fade(len(c)))
print(NAME, "written")
