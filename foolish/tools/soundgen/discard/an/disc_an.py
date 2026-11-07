"""Measure the discard reps (parts/, cut by an/cut.py) into the tables an/disc_syn.py builds from.
Bands: the 40 log bands of ../throwin/an/cs40.npy. Two-band envelopes: lo 150-1500 Hz, hi 1.5-15 kHz, 5 ms RMS.
SCRAPES vary in length (260-1200 ms), so their envelopes are on a time axis normalised to the stroke (onset 0, end 1, where
the 1-9 kHz level is 10 dB over the room); LIFTS and PLACEMENTS are aligned on their peak, in ms."""
import glob, wave, numpy as np
SR = 48000; cs = np.load("../throwin/an/cs40.npy")
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
def env(y, w=240): return 10 * np.log10(np.convolve(y ** 2, np.ones(w) / w, "same") + 1e-14)
def spec(y):
    n = 4096; Y = np.zeros(n // 2 + 1); c = 0
    for i in range(0, len(y) - n + 1, n // 4): Y += np.abs(np.fft.rfft(y[i:i + n] * np.hanning(n))) ** 2; c += 1
    if c == 0: Y = np.abs(np.fft.rfft(np.pad(y, (0, n - len(y))) * np.hanning(n))) ** 2; c = 1
    f = np.fft.rfftfreq(n, 1 / SR)
    return np.array([10 * np.log10(Y[(f >= q * 2 ** (-1 / 12)) & (f < q * 2 ** (1 / 12))].mean() / c + 1e-20) for q in cs])
room = load("parts/room.wav"); rs = spec(room)
def clean(s):   # take the room's power out of a rep's spectrum (the scrapes' lows are only 6-8 dB over it), floor at -12 dB SNR
    return 10 * np.log10(np.maximum(10 ** (s / 10) - 10 ** (rs / 10), 10 ** ((rs - 12) / 10)))
# scrapes and the pickup's insertion into the hand: on the stroke's own time axis
for cls, tag in (("scrape", "scr"), ("insert", "ins")):
  U = np.linspace(-0.1, 1.1, 49); SP, EL, EH, DUR, RAT = [], [], [], [], []
  for p in sorted(glob.glob(f"parts/{cls}_*.wav")):
      x = load(p); h = env(bp(x, 1000, 9000), 1440); fl = np.percentile(env(bp(room, 1000, 9000), 1440), 50)
      on = np.where(h > fl + 10)[0]; a, b = on[0], on[-1]; DUR.append((b - a) / SR)
      s = clean(spec(x[a:b])); SP.append(s - s.max())
      lo, hi = env(bp(x, 150, 1500)), env(bp(x, 1500, 15000)); ref = hi[a:b].max()
      t = (np.arange(len(x)) - a) / (b - a)
      EL.append(np.interp(U, t, lo - ref)); EH.append(np.interp(U, t, hi - ref))
      # texture: the hi band's 5 ms level about its own 50 ms smoothing, in dB (std) - how rough the scrape is
      sm = np.convolve(hi[a:b], np.ones(2400) / 2400, "same"); RAT.append(np.std((hi[a:b] - sm)[2400:-2400]))
  SCR_SPEC = np.median(SP, 0); SCR_EL = np.median(EL, 0); SCR_EH = np.median(EH, 0)
  print(f"{cls}: n", len(DUR), "dur ms", np.round(np.array(DUR) * 1000).astype(int), "median", int(np.median(DUR) * 1000))
  print("  roughness (std of 5 ms hi level about 50 ms smooth) dB", np.round(RAT, 1), "median", round(float(np.median(RAT)), 2))
  print("  spectrum dB re max, every 3rd band:", [(int(c), round(float(v), 1)) for c, v in zip(cs[::3], SCR_SPEC[::3])])
  print("  env hi (u, dB):", [(round(float(u), 2), round(float(v), 1)) for u, v in zip(U[::4], SCR_EH[::4])])
  print("  env lo (u, dB):", [(round(float(u), 2), round(float(v), 1)) for u, v in zip(U[::4], SCR_EL[::4])])
  np.save(f"an/{tag}_spec.npy", SCR_SPEC); np.save(f"an/{tag}_env.npy", np.stack([U, SCR_EL, SCR_EH])); np.save(f"an/{tag}_dur.npy", np.array(DUR))
# lifts and placements: aligned on the peak
TM = np.arange(-60, 301)
for name in ("lift", "place", "plift"):
    SPx, ELx, EHx = [], [], []
    for p in sorted(glob.glob(f"parts/{name}_*.wav")):
        x = load(p); f = bp(x, 150, 15000); w0 = int(0.13 * SR) if name == "plift" else 0   # a pickup's lift knock is at 0.15 s (scraping may run under it)
        pk = w0 + int(np.argmax(np.abs(f[w0: w0 + int(0.04 * SR)] if w0 else f)))
        lo, hi = env(bp(x, 150, 1500))[::48], env(bp(x, 1500, 15000))[::48]; k = pk // 48; ref = max(lo[k - 3:k + 4].max(), hi[k - 3:k + 4].max())
        ELx.append(np.array([lo[k + t] - ref if 0 <= k + t < len(lo) else np.nan for t in TM])); EHx.append(np.array([hi[k + t] - ref if 0 <= k + t < len(hi) else np.nan for t in TM]))
        s = clean(spec(x[pk - 480: pk + 4800])); SPx.append(s - s.max())
        # the knocks: peaks of the 0.25 ms hi envelope within 120 ms of the main one, over 10 dB above the local 10 ms
        fa = env(bp(x, 1500, 15000), 12); sl = env(bp(x, 1500, 15000), 480)
        ks = [i for i in range(max(48, pk - 2400), min(len(x) - 48, pk + 5760)) if fa[i] == fa[max(0, i - 48): i + 48].max() and fa[i] > sl[i] + 8 and fa[i] > fa[pk] - 20]
        print(f"{p}: knocks at ms re peak, dB re peak:", [(round((i - pk) / 48, 1), round(float(fa[i] - fa[pk]), 1)) for i in ks])
    sp = np.median(SPx, 0); el = np.nanmedian(ELx, 0); eh = np.nanmedian(EHx, 0)
    print(f"{name}: spectrum every 3rd band", [(int(c), round(float(v), 1)) for c, v in zip(cs[::3], sp[::3])])
    print(f"  env (ms, lo, hi):", [(int(t), round(float(a), 1), round(float(b), 1)) for t, a, b in zip(TM[::10], el[::10], eh[::10])])
    np.save(f"an/{name}_spec.npy", sp); np.save(f"an/{name}_env.npy", np.stack([TM, el, eh]))
