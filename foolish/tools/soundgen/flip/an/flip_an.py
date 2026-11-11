"""Flip reps 1-7 of IMG_2951 (parts/flip_k.wav, snap at 0.35 s): the slide before the snap, the snap's clicks, the
low "whup" pulses after it, and the beds. Writes the slide-only and whup-only cuts for measure.py and the tables."""
import wave, numpy as np
SR = 48000; S0 = int(0.35 * SR)
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
def env(y, w): return 20 * np.log10(np.sqrt(np.convolve(y ** 2, np.ones(w) / w, "same")) + 1e-12)
cs = np.load("an/cs40.npy")
def octs(y, n=4096):
    Y = np.abs(np.fft.rfft(np.pad(y, (0, max(0, n - len(y))))[:n] * np.hanning(n))) ** 2; f = np.fft.rfftfreq(n, 1 / SR)
    return np.array([10 * np.log10(Y[(f >= c * 2 ** (-1 / 12)) & (f < c * 2 ** (1 / 12))].mean() + 1e-20) for c in cs])
SL, SN, WH, BL, BH, SLE, LV = [], [], [], [], [], [], []
fade = lambda n: np.minimum(1, np.minimum(np.arange(n), n - 1 - np.arange(n)) / 96)
for k in range(1, 8):
    x = bp(load(f"parts/flip_{k}.wav"), 100, 15000)
    hi = env(bp(x, 2000, 12000), 240)[::48]; s0 = S0 // 48            # 5 ms RMS, 1 ms hop
    sp = int(np.argmax(hi[s0 - 250: s0 - 3])) + s0 - 250; spl = hi[sp]   # slide peak (before the snap)
    on = next(i for i in range(sp, 0, -1) if hi[i] < spl - 20)
    h12 = env(bp(x, 3000, 12000), 24); snl = h12[S0 - 96: S0 + 96].max()
    LV.append(20 * np.log10(np.abs(x).max()))
    # the slide's own envelope from onset to the snap, 10 points
    SLE.append([hi[on + int((s0 - on) * q)] - spl for q in np.linspace(0, 1, 11)])
    SL.append((s0 - on, sp - s0, spl - (snl - 0)))
    sl_cut = x[on * 48 - 2880: S0 - 96]; save(f"parts/fslide_{k}.wav", sl_cut * fade(len(sl_cut)))
    # snap clicks: 0.25 ms spikes >= 10 dB over the local median, within +-25 ms of the snap
    ck = []; i = S0 - 1200
    while i < S0 + 1200:
        if h12[i] > np.median(h12[i - 240:i + 240]) + 10 and h12[i] > snl - 25 and h12[i] == h12[i - 24:i + 25].max():
            ck.append(((i - S0) / 48, h12[i] - snl, sum(1 for j in range(i - 96, i + 96) if h12[j] > h12[i] - 10) / 48)); i += 24
        i += 1
    SN.append(ck)
    # whups: 150-800 Hz, 5 ms RMS peaks after the snap
    lo = bp(x, 150, 800); el = env(lo, 240)[::48]; top = el[s0: s0 + 300].max(); pk = []
    for j in range(s0 + 10, s0 + 300):
        if el[j] == el[j - 15:j + 16].max() and el[j] > top - 18 and el[j] - el[j - 15:j].min() > 4:
            a = j * 48; w = lo[a - 480:a + 480] * np.hanning(960); Sp = np.abs(np.fft.rfft(w, 16384)) ** 2; f = np.fft.rfftfreq(16384, 1 / SR); m = (f > 150) & (f < 800)
            fp = f[m][np.argmax(Sp[m])]; half = f[m][Sp[m] > Sp[m].max() / 4]          # -6 dB extent
            wid = sum(1 for q in range(j - 40, j + 40) if el[q] > el[j] - 10)
            pk.append((j - s0, el[j] - snl, fp, half.max() - half.min(), wid))
    WH.append(pk)
    wc = x[S0 + 240: S0 + int(0.25 * SR)]; save(f"parts/fwhup_{k}.wav", wc * fade(len(wc)))
    mask = np.ones(len(x), bool)
    for t, d, w in ck: c = S0 + int(t * 48); mask[c - 48: c + 120] = False
    for arr, a, b in ((BL, 150, 1000), (BH, 2000, 12000)):
        e = env(np.where(mask, bp(x, a, b), 0), 240); arr.append([e[S0 + int(t * 48)] - snl for t in (-80, -40, -10, 5, 20, 40, 60, 90, 130, 180, 250)])
    print(f"flip {k}: peak {LV[-1]:.1f} dBFS; slide {s0-on} ms long, its peak {sp-s0:+d} ms re snap at {spl-snl:+.1f} dB re snap")
    print("   snap clicks (ms:dB:width) " + "  ".join(f"{t:+.1f}:{d:.0f}:{w:.2f}" for t, d, w in ck))
    print("   whups (ms:dB re snap:Hz:-6dB bw Hz:-10dB width ms) " + "  ".join(f"+{t}:{d:.0f}:{f:.0f}:{b:.0f}:{w}" for t, d, f, b, w in pk))
S = np.array(SL); print(f"\nslide length {np.median(S[:,0]):.0f} ms ({S[:,0].min()}-{S[:,0].max()}); peak at {np.median(S[:,1]):.0f} ms re snap; slide peak re snap click {np.median(S[:,2]):.1f} dB ({S[:,2].min():.1f}..{S[:,2].max():.1f})")
print("slide env re its peak at 0..1 of onset->snap: " + " ".join(f"{v:.1f}" for v in np.median(SLE, 0)))
W = [w for r in WH for w in r]; print(f"whups: {len(W)} in 7 flips; first at +{np.median([r[0][0] for r in WH if r]):.0f} ms; level re snap {np.median([w[1] for w in W]):.1f} dB; Hz {min(w[2] for w in W):.0f}-{max(w[2] for w in W):.0f}; bw median {np.median([w[3] for w in W]):.0f} Hz; width median {np.median([w[4] for w in W]):.0f} ms")
T = [-80, -40, -10, 5, 20, 40, 60, 90, 130, 180, 250]
print("bed re snap, ms:  " + " ".join(f"{t:6d}" for t in T)); print("150-1k:           " + " ".join(f"{v:6.1f}" for v in np.median(BL, 0))); print("2-12k:            " + " ".join(f"{v:6.1f}" for v in np.median(BH, 0)))
