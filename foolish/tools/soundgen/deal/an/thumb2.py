"""Thumb slide v3 (prototype): noise coloured by the measured average spectrum of Alex's five clean slides
(1/12 octave: the low body 200 Hz-1 kHz, the scooped 1.5-5 kHz, the 7-9 kHz peak), under the friction envelope
(rise, plateau ~6 dB down, a catch where the edge clears, fall) and texture (stick-slip wobble, micro-catches)."""
import wave, numpy as np
rng = np.random.default_rng(7); SR = 48000
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
# the template: average 1/12-octave spectrum of the real slides' active parts
cs = np.geomspace(100, 20000, 160); specs = []
for i in range(1, 6):
    x = load(f"parts/real_thumb_{i}.wav"); h = 48; e = np.sqrt(np.convolve(x ** 2, np.ones(h) / h, "same")); a = np.where(e > e.max() * 0.2)[0]; y = x[a[0]:a[-1]]
    n = 8192; y = np.pad(y, (0, max(0, n - len(y))))[:n]; S = np.abs(np.fft.rfft(y * np.hanning(n))) ** 2; f = np.fft.rfftfreq(n, 1 / SR)
    specs.append(np.array([10 * np.log10(S[(f >= c * 2 ** (-1 / 24)) & (f < c * 2 ** (1 / 24))].mean() + 1e-20) for c in cs]))
tmpl = np.mean(specs, 0); tmpl -= tmpl.max()
def colour(n):
    X = np.fft.rfft(rng.standard_normal(n)); f = np.fft.rfftfreq(n, 1 / SR)
    g = np.interp(np.log(np.maximum(f, 1)), np.log(cs), tmpl); g[f < 120] = -60
    return np.fft.irfft(X * 10 ** (g / 20), n)
def lp(x, fc):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[f > fc] = 0; return np.fft.irfft(X, len(x))
def thumb():
    rise, plat, fall = rng.uniform(.03, .05), rng.uniform(.06, .10), rng.uniform(.04, .06); total = rise + plat + fall
    bump_at, bump_w = rise + plat * rng.uniform(.2, .9), rng.uniform(.02, .03)
    n = int((total + 0.08) * SR); t = np.arange(n) / SR
    env = np.where(t < rise, 10 ** ((-18 + 12 * t / rise) / 20), np.where(t < rise + plat, 10 ** (-6 / 20), 10 ** ((-6 - 14 * (t - rise - plat) / fall) / 20)))
    env[t >= total] = 10 ** (-20 / 20) * np.exp(-(t[t >= total] - total) / 0.012)       # the last of it dies away
    d = (t - bump_at) / (bump_w / 2); env = np.maximum(env, np.where(np.abs(d) < 1, 10 ** ((-6 + 6 * (1 - d ** 2)) / 20), 0))
    wob = lp(rng.standard_normal(n), rng.uniform(30, 150)); wob = 1 + rng.uniform(.3, .45) * wob / (wob.std() + 1e-9) * 0.35
    catches = np.ones(n); k = int(rng.uniform(.012, .025) * SR)
    while k < n: L = int(rng.uniform(.001, .002) * SR); catches[k:k + L] = 10 ** (rng.uniform(3, 6) / 20); k += L + int(rng.uniform(.0125, .025) * SR)
    y = colour(n) * env * np.maximum(wob, 0.1) * catches
    pad = np.zeros(int(0.02 * SR)); y = np.concatenate([pad, y, pad])
    return y / np.abs(y).max() * 10 ** (rng.uniform(-41.5, -35.5) / 20)
for v in range(1, 9): save(f"synth/thumb3_{v:02d}.wav", thumb())
print("template (dB re max) at 250/500/1k/2k/4k/4.5k/8k/12k Hz:", " ".join(f"{np.interp(np.log(c), np.log(cs), tmpl):.0f}" for c in (250, 500, 1000, 2000, 4000, 4500, 8000, 12000)))

# v4: the slide's AIR-GAP resonances gliding up (LPC tracks of the real slides: ~0.5-0.9 -> 1.0-1.4 kHz and
# ~1.6-2.0 -> 3.0-3.5 kHz over the slide): noise through time-varying two-pole resonators, under the same envelope
def glide_res(n, f0, f1, t_start, bw, sr=SR):
    y = np.zeros(n); x = rng.standard_normal(n); y1 = y2 = 0.0
    for i in range(n):
        u = min(1.0, max(0.0, (i / n - t_start) / (1 - t_start)))
        f = f0 * (f1 / f0) ** (u ** 1.3)                                  # a rising glide, slow then faster (the tone)
        r = np.exp(-np.pi * bw / sr); c = 2 * r * np.cos(2 * np.pi * f / sr)
        v = x[i] * (1 - r) + c * y1 - r * r * y2; y2, y1 = y1, v; y[i] = v
    return y / (y.std() + 1e-12)
def thumb4():
    rise, plat, fall = rng.uniform(.03, .05), rng.uniform(.06, .10), rng.uniform(.04, .06); total = rise + plat + fall
    bump_at, bump_w = rise + plat * rng.uniform(.2, .9), rng.uniform(.02, .03)
    n = int((total + 0.08) * SR); t = np.arange(n) / SR
    env = np.where(t < rise, 10 ** ((-18 + 12 * t / rise) / 20), np.where(t < rise + plat, 10 ** (-6 / 20), 10 ** ((-6 - 14 * (t - rise - plat) / fall) / 20)))
    env[t >= total] = 10 ** (-20 / 20) * np.exp(-(t[t >= total] - total) / 0.012)
    d = (t - bump_at) / (bump_w / 2); env = np.maximum(env, np.where(np.abs(d) < 1, 10 ** ((-6 + 6 * (1 - d ** 2)) / 20), 0))
    wob = lp(rng.standard_normal(n), rng.uniform(30, 150)); wob = 1 + rng.uniform(.3, .45) * wob / (wob.std() + 1e-9) * 0.35
    m = int((total) * SR); ts = rng.uniform(0.25, 0.45)
    r1 = np.zeros(n); r1[:m] = glide_res(m, rng.uniform(500, 900), rng.uniform(1000, 1400), ts, rng.uniform(150, 300))
    r2 = np.zeros(n); r2[:m] = glide_res(m, rng.uniform(1600, 2000), rng.uniform(3000, 3500), ts, rng.uniform(200, 400))
    hiss = colour(n); res = rng.uniform(0.6, 1.0) * r1 + rng.uniform(0.4, 0.8) * r2
    y = (hiss / (hiss.std() + 1e-12) + res * rng.uniform(0.8, 1.3)) * env * np.maximum(wob, 0.1)
    pad = np.zeros(int(0.02 * SR)); y = np.concatenate([pad, y, pad])
    return y / np.abs(y).max() * 10 ** (rng.uniform(-41.5, -35.5) / 20)
for v in range(1, 9): save(f"synth/thumb4_{v:02d}.wav", thumb4())
print("thumb4 written")

# v5: Alex's idea - the thumb ACCELERATES. Friction as a needle on a record: a fixed random surface texture read
# at a position that advances with an accelerating velocity, so the spectrum scales up with speed and the level
# with it; then coloured by the slides' measured (hollow) body spectrum.
def colour_spec(y):
    n = len(y); X = np.fft.rfft(y); f = np.fft.rfftfreq(n, 1 / SR)
    g = np.interp(np.log(np.maximum(f, 1)), np.log(cs), tmpl); g[f < 120] = -60
    # the template already holds the average friction colour; divide out a typical source tilt so the body is not counted twice
    return np.fft.irfft(X * 10 ** (g / 20) / np.maximum(np.sqrt(np.maximum(f, 200) / 2000), 1e-3), n)
def thumb5():
    dur = rng.uniform(0.13, 0.19); n = int(dur * SR); t = np.arange(n) / SR
    v0, v1 = 1.0, rng.uniform(1.8, 3.0)                                   # speed rises 1.8-3x through the slide
    v = v0 + (v1 - v0) * (t / dur) ** rng.uniform(1.2, 2.0)
    x = np.cumsum(v) / SR                                                 # position along the surface (arbitrary units)
    L = int(x[-1] * 40000) + 10; surf = np.cumsum(rng.standard_normal(L)); surf -= np.convolve(surf, np.ones(200) / 200, "same")   # rough, no drift
    p = np.interp(x * 40000, np.arange(L), surf); y = np.gradient(p) * SR / 40000          # the needle's signal: d(profile)/dt, so it scales with speed
    press = np.minimum(1, t / rng.uniform(0.02, 0.04)) * np.minimum(1, (dur - t) / rng.uniform(0.015, 0.03))   # contact pressure: on, then the card clears
    y = y * press
    n2 = n + int(0.06 * SR); y = np.concatenate([y, np.zeros(n2 - n)])
    y[n:] = 0; y = colour_spec(y)
    tail = np.exp(-np.arange(n2) / SR / 0.012); y[n:] *= tail[:n2 - n]
    pad = np.zeros(int(0.02 * SR)); y = np.concatenate([pad, y, pad])
    return y / np.abs(y).max() * 10 ** (rng.uniform(-41.5, -35.5) / 20)
for v in range(1, 9): save(f"synth/thumb5_{v:02d}.wav", thumb5())
print("thumb5 written")

# v6: thumb4 with the measured pitch tracks (an/pitch.py, median of the five real slides):
#   low resonance 1436 -> 1051 -> 1305 Hz (a V: dips, then rises), high 2627 -> 3075 -> 3244 Hz (rises); each +-8%
def track_res(n, f_of_u, bw, sr=SR):
    y = np.zeros(n); x = rng.standard_normal(n); y1 = y2 = 0.0; r = np.exp(-np.pi * bw / sr)
    for i in range(n):
        f = f_of_u(i / n); c = 2 * r * np.cos(2 * np.pi * f / sr)
        v = x[i] * (1 - r) + c * y1 - r * r * y2; y2, y1 = y1, v; y[i] = v
    return y / (y.std() + 1e-12)
WOB = 0.12
def thumb6():
    rise, plat, fall = rng.uniform(.03, .05), rng.uniform(.06, .10), rng.uniform(.04, .06); total = rise + plat + fall
    bump_at, bump_w = rise + plat * rng.uniform(.2, .9), rng.uniform(.02, .03)
    n = int((total + 0.08) * SR); t = np.arange(n) / SR
    env = np.where(t < rise, 10 ** ((-18 + 12 * t / rise) / 20), np.where(t < rise + plat, 10 ** (-6 / 20), 10 ** ((-6 - 14 * (t - rise - plat) / fall) / 20)))
    env[t >= total] = 10 ** (-20 / 20) * np.exp(-(t[t >= total] - total) / 0.012)
    d = (t - bump_at) / (bump_w / 2); env = np.maximum(env, np.where(np.abs(d) < 1, 10 ** ((-6 + 6 * (1 - d ** 2)) / 20), 0))
    wob = lp(rng.standard_normal(n), rng.uniform(15, 50)); wob = 1 + WOB * wob / (wob.std() + 1e-9)   # slower, gentler stick-slip (less sandpaper)
    j = lambda: rng.uniform(0.92, 1.08)
    ls, lm, le = 1850 * j(), 1000 * j(), 1700 * j()   # aimed past the targets: the hiss pulls the measured peak toward the middle
    hs, hm, he = 2627 * j(), 3075 * j(), 3244 * j()
    def lowf(u): return lm + (ls - lm) * (1 - 2 * u) ** 2 if u < 0.5 else lm + (le - lm) * (2 * u - 1) ** 2
    def highf(u): return hs + (hm - hs) * (2 * u) if u < 0.5 else hm + (he - hm) * (2 * u - 1)
    m = int(total * SR)
    r1 = np.zeros(n); r1[:m] = track_res(m, lowf, rng.uniform(100, 180))
    r2 = np.zeros(n); r2[:m] = track_res(m, highf, rng.uniform(200, 400))
    hiss = colour(n); res = rng.uniform(0.7, 1.0) * r1 + rng.uniform(0.35, 0.6) * r2
    y = (hiss / (hiss.std() + 1e-12) + res * rng.uniform(0.45, 0.7)) * env * np.maximum(wob, 0.1)
    pad = np.zeros(int(0.02 * SR)); y = np.concatenate([pad, y, pad])
    return y / np.abs(y).max() * 10 ** (rng.uniform(-41.5, -35.5) / 20)
for v in range(1, 9): save(f"synth/thumb6_{v:02d}.wav", thumb6())
print("thumb6 written")

# v7: thumb6's tones and texture under Alex's MEASURED ADSR (an/adsr.py, five real slides):
#   attack 10->90% 82-107 ms (a slow swell to a late peak), decay 10-19 ms to a sustain 4-8 dB down held 33-83 ms,
#   release 92-116 ms falling an even ~2.1 dB per 10 ms (1.7-2.4), then on into silence
def adsr_env(n):
    A = rng.uniform(.065, .090) / 0.8; D = rng.uniform(.010, .019); S = -rng.uniform(4, 8); SL = rng.uniform(.005, .035)   # the measured 33-83 ms includes ~20 ms of release above -10 dB
    slope = rng.uniform(1.6, 2.1) / 0.010                                # dB per second
    t = np.arange(n) / SR; e = np.zeros(n)
    a = t < A; e[a] = (t[a] / A)                                          # linear amplitude ramp: 10->90% takes 0.8 A
    d = (t >= A) & (t < A + D); e[d] = 10 ** ((S * (t[d] - A) / D) / 20)
    s = (t >= A + D) & (t < A + D + SL); e[s] = 10 ** (S / 20)
    r = t >= A + D + SL; e[r] = 10 ** ((S - slope * (t[r] - A - D - SL)) / 20)
    return e, A + D + SL + 40 / slope                                     # where it is ~40 dB down
def thumb7():
    n = int(0.42 * SR); env, end = adsr_env(n); n = min(n, int((end + 0.01) * SR)); env = env[:n]
    wob = lp(rng.standard_normal(n), rng.uniform(15, 50)); wob = 1 + WOB * wob / (wob.std() + 1e-9)
    j = lambda: rng.uniform(0.92, 1.08)
    ls, lm, le = 1850 * j(), 1000 * j(), 1700 * j(); hs, hm, he = 2627 * j(), 3075 * j(), 3244 * j()
    def lowf(u): return lm + (ls - lm) * (1 - 2 * u) ** 2 if u < 0.5 else lm + (le - lm) * (2 * u - 1) ** 2
    def highf(u): return hs + (hm - hs) * (2 * u) if u < 0.5 else hm + (he - hm) * (2 * u - 1)
    r1 = track_res(n, lowf, rng.uniform(100, 180)); r2 = track_res(n, highf, rng.uniform(200, 400))
    hiss = colour(n); res = rng.uniform(0.7, 1.0) * r1 + rng.uniform(0.35, 0.6) * r2
    y = (hiss / (hiss.std() + 1e-12) + res * rng.uniform(0.45, 0.7)) * env * np.maximum(wob, 0.1)
    pad = np.zeros(int(0.02 * SR)); y = np.concatenate([pad, y, pad])
    return y / np.abs(y).max() * 10 ** (rng.uniform(-41.5, -35.5) / 20)
for v in range(1, 9): save(f"synth/thumb7_{v:02d}.wav", thumb7())
print("thumb7 written")

# v8: the envelope IS the real slides' median curve (an/envcmp.py, 10 ms RMS of 1-15 kHz, dB re peak):
#   a long swell (-23 dB at -120 ms ... -7 dB at -20 ms), a 20-30 ms catch ~6 dB above it at 0, then ~2.3 dB/10 ms down;
#   below -28 dB (the recordings' room floor) it keeps the same slope. Per variant: time x0.85-1.15, +-1.5 dB per point.
ENV_T = np.array([-150, -120, -100, -80, -60, -40, -20, -8, 0, 8, 20, 40, 60, 80, 100, 120, 160, 200]) / 1000
ENV_D = np.array([-30, -23.1, -18.8, -12.2, -10.5, -7.7, -6.7, -5.5, 0.0, -3.0, -5.2, -8.9, -14.6, -16.8, -22.4, -25.6, -34.0, -42.0])
def env_v8(n):
    ts = ENV_T * rng.uniform(0.85, 1.15); dd = ENV_D + rng.uniform(-1.5, 1.5, len(ENV_D)); dd[8] = 0
    sp = rng.uniform(-0.008, 0.008); ts[7:10] += sp                      # the catch moves a little
    t = np.arange(n) / SR + ts[0]
    return 10 ** (np.interp(t, ts, dd, left=-60, right=-60) / 20), ts[-1] - ts[0]
AIR = 1.45   # the 5-10 kHz air, +3 dB on the hiss against the resonances
def thumb8():
    n = int(0.42 * SR); env, span = env_v8(n); n = int((span + 0.01) * SR); env = env[:n]
    wob = lp(rng.standard_normal(n), rng.uniform(15, 50)); wob = 1 + WOB * wob / (wob.std() + 1e-9)
    j = lambda: rng.uniform(0.92, 1.08)
    ls, lm, le = 1850 * j(), 1000 * j(), 1700 * j(); hs, hm, he = 2627 * j(), 3075 * j(), 3244 * j()
    def lowf(u): return lm + (ls - lm) * (1 - 2 * u) ** 2 if u < 0.5 else lm + (le - lm) * (2 * u - 1) ** 2
    def highf(u): return hs + (hm - hs) * (2 * u) if u < 0.5 else hm + (he - hm) * (2 * u - 1)
    r1 = track_res(n, lowf, rng.uniform(100, 180)); r2 = track_res(n, highf, rng.uniform(450, 700))   # broader: shapes the tone without a scratchy band
    hiss = colour(n); res = rng.uniform(0.6, 0.85) * r1 + rng.uniform(0.18, 0.3) * r2
    y = (hiss / (hiss.std() + 1e-12) * AIR + res * rng.uniform(0.4, 0.6)) * env * np.maximum(wob, 0.1)
    pad = np.zeros(int(0.02 * SR)); y = np.concatenate([pad, y, pad])
    return y / np.abs(y).max() * 10 ** (rng.uniform(-41.5, -35.5) / 20)
for v in range(1, 9): save(f"synth/thumb8_{v:02d}.wav", thumb8())
print("thumb8 written")

# v9 = v8 with (1) the low resonance's V timed against the ENVELOPE: start/dip/end across the audible part
# (envelope within 20 dB of its peak), so the rising end lands where the energy is; (2) the low resonator a
# little broader and quieter (it stood ~2 dB proud at 1 kHz)
def track_res_arr(farr, bw, sr=SR):
    n = len(farr); y = np.zeros(n); x = rng.standard_normal(n); y1 = y2 = 0.0; r = np.exp(-np.pi * bw / sr)
    for i in range(n):
        c = 2 * r * np.cos(2 * np.pi * farr[i] / sr); v = x[i] * (1 - r) + c * y1 - r * r * y2; y2, y1 = y1, v; y[i] = v
    return y / (y.std() + 1e-12)
def thumb9():
    n = int(0.42 * SR); env, span = env_v8(n); n = int((span + 0.01) * SR); env = env[:n]
    wob = lp(rng.standard_normal(n), rng.uniform(15, 50)); wob = 1 + WOB * wob / (wob.std() + 1e-9)
    act = np.where(env > env.max() * 10 ** (-20 / 20))[0]; a0, a1 = act[0], act[-1]
    u = np.clip((np.arange(n) - a0) / max(1, a1 - a0), 0, 1)
    j = lambda: rng.uniform(0.92, 1.08)
    ls, lm, le = LOW_V[0] * j(), LOW_V[1] * j(), LOW_V[2] * j(); hs, hm, he = HIGH_V[0] * j(), HIGH_V[1] * j(), HIGH_V[2] * j()
    low = np.where(u < 0.5, lm + (ls - lm) * (1 - 2 * u) ** 2, lm + (le - lm) * (2 * u - 1) ** 2)
    high = np.where(u < 0.5, hs + (hm - hs) * 2 * u, hm + (he - hm) * (2 * u - 1))
    r1 = track_res_arr(low, rng.uniform(160, 260)); r2 = track_res_arr(high, rng.uniform(450, 700))
    hiss = colour(n); res = rng.uniform(0.33, 0.48) * r1 + rng.uniform(0.18, 0.3) * r2
    y = (hiss / (hiss.std() + 1e-12) * AIR + res * rng.uniform(0.4, 0.6)) * env * np.maximum(wob, 0.1)
    pad = np.zeros(int(0.02 * SR)); y = np.concatenate([pad, y, pad])
    return y / np.abs(y).max() * 10 ** (rng.uniform(-41.5, -35.5) / 20)
LOW_V = (2050, 1000, 1850)
HIGH_V = (2200, 2950, 3200)
for v in range(1, 9): save(f"synth/thumb9_{v:02d}.wav", thumb9())
print("thumb9 written")
