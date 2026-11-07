"""The discard's three parts, from an/disc_an.py's tables (Alex's IMG_3046-3048, 2026-11-07).
SCRAPE: one stroke of the hand sweeping the table's cards together: noise in the measured spectrum (low and full: the
  pile's body sits at 300 Hz, 11 dB down by 1.5 kHz; a single card's pass is 10 dB thinner below 1 kHz), on the measured
  two-band envelope over the stroke's own length (a 10% swell, a held middle peaking at 40-60%, a fall over the last 20%),
  with the measured roughness (the 5 ms level wanders ~2.3 dB about its 50 ms average: many card edges catching) and
  card-edge ticks.
LIFT: the pile picked up and turned: a sharp knock (the pile's edge on the table), a second softer one 35-62 ms later as
  it tips over, a short rustle.
PLACE: the pile set softly on the discard pile: a low, soft landing (its 150 Hz end 25 dB over its 2 kHz), a second touch
  15-75 ms later as it settles.
INSERT and PLIFT: the pickup's slide into the hand and its one soft lift knock (IMG_3052-3054).
Usage (from ../discard): python3 -I an/disc_syn.py NAME SEED [fit]   ('fit' re-measures the spectral fit first)"""
import sys, os, glob, wave, numpy as np
SR = 48000; HERE = os.path.dirname(os.path.abspath(__file__))
NAME = sys.argv[1] if len(sys.argv) > 1 else "disc1"; SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 1
cs = np.load(f"{HERE}/../../throwin/an/cs40.npy")
SCR = np.load(f"{HERE}/scr_spec.npy"); SCRE = np.load(f"{HERE}/scr_env.npy"); SCRD = np.load(f"{HERE}/scr_dur.npy")
LIFT = np.load(f"{HERE}/lift_spec.npy"); LIFTE = np.load(f"{HERE}/lift_env.npy")
PLACE = np.load(f"{HERE}/place_spec.npy"); PLACEE = np.load(f"{HERE}/place_env.npy")
# the pickup (IMG_3052-3054): the phone was in the hand that takes the cards, so its lows under 280 Hz are handling rumble;
# they are held at the 301 Hz band's level
def no_rumble(v): v = v.copy(); v[cs < 280] = np.minimum(v[cs < 280], v[cs >= 280][0]); return v
INS = no_rumble(np.load(f"{HERE}/ins_spec.npy")); INSE = np.load(f"{HERE}/ins_env.npy"); INSD = np.load(f"{HERE}/ins_dur.npy")
PLIFT = no_rumble(np.load(f"{HERE}/plift_spec.npy")); PLIFTE = np.load(f"{HERE}/plift_env.npy")
ROUGH = 3.0    # dB std of the extra slow level wander (calibrated against measure.py's 'rough 1/20ms' of the reps)
def fit(name):
    try: return np.load(f"{HERE}/{name}_fit.npy")
    except OSError: return np.zeros(40)
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
def eq(y, tmpl):
    n = len(y); X = np.fft.rfft(y); f = np.fft.rfftfreq(n, 1 / SR); g = np.interp(np.log(np.maximum(f, 1)), np.log(cs), tmpl); g[f < 120] = -60; g[f > 15000] = -60
    return np.fft.irfft(X * 10 ** (g / 20), n)
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
def smooth(v, k=5):
    v = np.where(np.isnan(v), -60, v); return np.convolve(np.pad(v, k // 2, mode="edge"), np.ones(k) / k, "valid")
def two_band(rng, n, tmpl, el, eh):
    """Noise in the template, split at 1.5 kHz, each band on its own dB envelope (per sample)."""
    lo = bp(eq(rng.standard_normal(n), np.where(cs < 1500, tmpl, -60)), 150, 1500); lo /= np.sqrt(np.mean(lo ** 2))
    hi = bp(eq(rng.standard_normal(n), np.where(cs >= 1500, tmpl, -60)), 1500, 15000); hi /= np.sqrt(np.mean(hi ** 2))
    return lo * 10 ** (el / 20) + hi * 10 ** (eh / 20)
def strike(rng, n, at, db, width, tmpl):
    k = int(at * SR); L = int(width * 3 * SR) + 8; t = np.arange(L) / SR
    z = np.zeros(n); z[k:k + L] = rng.standard_normal(L) * np.minimum(1, t / 0.0002) * np.exp(-t / (width / 2.3)); z = eq(z, tmpl)
    return z / (np.abs(z).max() + 1e-12) * 10 ** (db / 20)
def wander(rng, n, std, corr):
    """A slow random level in dB: noise smoothed over `corr` seconds, scaled to `std`."""
    w = int(corr * SR); r = np.convolve(rng.standard_normal(n + w), np.ones(w) / w, "valid")[:n]; return r / (r.std() + 1e-12) * std
def scrape(rng, dur=None, pre=0.06, post=0.12, spec=None, envt=None, durs=None, fitname="scr", rough=None):
    """One stroke; its onset at `pre` s. dur: the stroke's length (default: drawn from the reps' 250-940 ms).
    The other arguments swap in another stroke-like sound's tables (the pickup's slide into the hand)."""
    spec = SCR if spec is None else spec; envt = SCRE if envt is None else envt; durs = SCRD if durs is None else durs
    if dur is None: dur = float(np.clip(rng.choice(durs) * rng.uniform(0.9, 1.1), 0.25, 0.95))
    n = int((pre + dur + post) * SR); u = (np.arange(n) / SR - pre) / dur
    U, EL, EH = envt[0], smooth(envt[1], 3), smooth(envt[2], 3)
    jl, jh = rng.uniform(-1.5, 1.5, len(U)), rng.uniform(-1.5, 1.5, len(U))
    el = np.interp(u, U, EL + jl, left=-70, right=-70); eh = np.interp(u, U, EH + jh, left=-70, right=-70)
    wv = wander(rng, n, ROUGH if rough is None else rough, 0.008); tmpl = spec + fit(fitname)
    y = two_band(rng, n, tmpl, el + 0.6 * wv, eh + wv)
    loc = np.sqrt(np.convolve(bp(y, 3000, 15000) ** 2, np.ones(480) / 480, "same"))
    for _ in range(rng.poisson(14 * dur)):             # card edges catching: the reps show ~6 clicks a stroke (measure.py)
        a = pre + rng.uniform(0.02, 1.0) * dur; k = int(a * SR)
        y += strike(rng, n, a, 0, rng.uniform(0.001, 0.0025), np.where(cs >= 1500, tmpl, tmpl - 12)) * loc[k] * 10 ** (rng.uniform(6, 13) / 20) * 3
    return y
def event(rng, tmpl_name, spec, envt, knocks, kg, pre=0.08, post=0.32):
    """A lift or a placement: the measured two-band envelope round its peak at `pre`, plus its knocks."""
    n = int((pre + post) * SR); tt = (np.arange(n) / SR - pre) * 1000
    TM, EL, EH = envt[0], smooth(envt[1]), smooth(envt[2])
    ts = rng.uniform(0.95, 1.25); jl, jh = rng.uniform(-1.5, 1.5, len(TM)), rng.uniform(-1.5, 1.5, len(TM))
    el = np.interp(tt, TM * ts, EL + jl, left=-70, right=-70); eh = np.interp(tt, TM * ts, EH + jh, left=-70, right=-70)
    tmpl = spec + fit(tmpl_name); y = two_band(rng, n, tmpl, el, eh); ref = np.sqrt(np.mean(y[int(pre * SR) - 240: int(pre * SR) + 240] ** 2))
    for at, db, w in knocks: y += strike(rng, n, pre + at, 0, w, tmpl) * ref * 10 ** (db / 20) * kg   # kg: the knocks over the bed
    return y
def ticks(rng, count, t0, t1, lo, hi):
    """Small edge ticks between t0 and t1 s re the peak, lo..hi dB re the main knock."""
    return [(rng.uniform(t0, t1), rng.uniform(lo, hi), rng.uniform(0.0005, 0.0012)) for _ in range(count)]
def lift(rng):
    k = [(0.0, rng.uniform(0, 2), rng.uniform(0.0008, 0.0015)), (rng.uniform(0.035, 0.062), rng.uniform(-10, -6), rng.uniform(0.001, 0.002))]
    return event(rng, "lift", LIFT, LIFTE, k + ticks(rng, int(rng.integers(2, 4)), -0.025, 0.11, -14, -6), 4)   # the reps: 3-5 clicks over ~66 ms
def place(rng):
    k = [(0.0, rng.uniform(-2, 0), rng.uniform(0.0012, 0.002)), (rng.uniform(0.015, 0.075), rng.uniform(-13, -5), rng.uniform(0.0012, 0.002))]
    return event(rng, "place", PLACE, PLACEE, k + ticks(rng, int(rng.integers(4, 8)), 0.0, 0.3, -20, -10), 7)   # the reps: 5-11 sharp clicks (1-4 kHz kurtosis ~20) as the cards settle
def insert(rng, dur=None):
    """The pickup's last sound: the cards slid into the fanned hand, one 350-750 ms slide, brighter than a scrape and
    swelling to its end (the cards meeting the stop). Its roughness measured 3.8 dB."""
    return scrape(rng, dur, spec=INS, envt=INSE, durs=INSD, fitname="ins", rough=ROUGH * 1.3)
def plift(rng):
    """The pickup's lift: one soft knock as the pile leaves the table (no turn-over knock, unlike the discard)."""
    return event(rng, "plift", PLIFT, PLIFTE, [(0.0, rng.uniform(-1, 1), rng.uniform(0.0008, 0.0015))] + ticks(rng, int(rng.integers(0, 3)), -0.02, 0.08, -18, -10), 3)
def norm(y, db): return y / (np.abs(bp(y, 150, 15000)).max() + 1e-12) * 10 ** (db / 20)
def oct12(y):   # as ../throwin/an/measure.py
    y = bp(y, 150, 15000); n = 8192; Y = np.zeros(n // 2 + 1); y = np.pad(y, (0, max(0, n - len(y)))); c = 0
    for i in range(0, len(y) - n + 1, n // 4): Y += np.abs(np.fft.rfft(y[i:i + n] * np.hanning(n))) ** 2; c += 1
    f = np.fft.rfftfreq(n, 1 / SR); v = np.array([10 * np.log10(Y[(f >= q * 2 ** (-1 / 24)) & (f < q * 2 ** (1 / 24))].mean() / c + 1e-20) for q in cs])
    return v - v[(cs > 300) & (cs < 3000)].mean()   # re the 0.3-3 kHz mean, so a rumbly low band cannot move the rest
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
if __name__ == "__main__" and "fit" in sys.argv[3:]:
    for part, gen, name in ((scrape, "scrape", "scr"), (lift, "lift", "lift"), (place, "place", "place"), (insert, "insert", "ins"), (plift, "plift", "plift")):
        A = np.median([oct12(load(p)) for p in glob.glob(f"parts/{gen}_*.wav")], 0)
        for it in range(3):
            rng = np.random.default_rng(99); B = np.median([oct12(part(rng)) for _ in range(12)], 0)
            d = A - B
            if gen in ("insert", "plift"): d[cs < 280] = 0      # the handling rumble is not fitted
            d = np.convolve(np.pad(d, 1, mode="edge"), np.ones(3) / 3, "valid"); np.save(f"{HERE}/{name}_fit.npy", fit(name) + d)
            print(gen, "fit pass", it, "max |A-B|", round(float(np.abs(A - B).max()), 1))
if __name__ == "__main__":
    rng = np.random.default_rng(SEED)
    for v in range(1, 13):
        save(f"synth/{NAME}_scrape_{v:02d}.wav", norm(scrape(rng), -36))
        save(f"synth/{NAME}_lift_{v:02d}.wav", norm(lift(rng), -22))
        save(f"synth/{NAME}_place_{v:02d}.wav", norm(place(rng), -31))
        save(f"synth/{NAME}_insert_{v:02d}.wav", norm(insert(rng), -40))
        save(f"synth/{NAME}_plift_{v:02d}.wav", norm(plift(rng), -40))
    print(NAME, "written")
