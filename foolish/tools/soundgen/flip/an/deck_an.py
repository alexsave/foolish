"""Deck set down on a card, IMG_2950 placements 1-6 (parts/deck_k.wav, peak at 0.2 s): the pre-touch, the thud's
contacts, the low and high beds over time (the settle), and the contact / bed spectra."""
import wave, numpy as np
SR = 48000; P0 = int(0.2 * SR)
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
def env(y, w): return 20 * np.log10(np.sqrt(np.convolve(y ** 2, np.ones(w) / w, "same")) + 1e-12)
cs = np.load("an/cs40.npy")
def octs(y, n=4096):
    Y = np.abs(np.fft.rfft(np.pad(y, (0, max(0, n - len(y))))[:n] * np.hanning(n))) ** 2; f = np.fft.rfftfreq(n, 1 / SR)
    return np.array([10 * np.log10(Y[(f >= c * 2 ** (-1 / 12)) & (f < c * 2 ** (1 / 12))].mean() + 1e-20) for c in cs])
T = [-80, -60, -40, -20, -8, -3, 0, 3, 8, 15, 25, 40, 60, 90, 130, 180, 250, 350]
CK, CS, BS, BL, BH, PEAK = [], [], [], [], [], []
for k in range(1, 7):
    x = bp(load(f"parts/deck_{k}.wav"), 100, 15000); PEAK.append(20 * np.log10(np.abs(x).max()))
    h = env(bp(x, 3000, 12000), 12); w0 = P0 - int(0.12 * SR); top = h[w0: P0 + int(0.08 * SR)].max(); loud = P0; ck = []; i = w0
    thud = env(x, 240)[P0 - 240: P0 + 240].max()
    while i < P0 + int(0.08 * SR):
        if h[i] > np.median(h[i - 240:i + 240]) + 10 and h[i] > top - 30 and h[i] == h[i - 24:i + 25].max():
            ck.append(((i - loud) / 48, h[i] - top, sum(1 for j in range(i - 96, i + 96) if h[j] > h[i] - 10) / 48)); i += 24
        i += 1
    CK.append(ck); mask = np.ones(len(x), bool)
    for t, d, w in ck:
        c = loud + int(t * 48); CS.append(octs(x[c - 24:c + 48]) - octs(x[c - 24:c + 48]).max()); mask[c - 48:c + 144] = False
    BS.append(octs(np.where(mask, x, 0)[loud: loud + int(0.3 * SR)], 8192))
    for arr, a, b in ((BL, 100, 1000), (BH, 2000, 12000)):
        e = env(np.where(mask, bp(x, a, b), 0), 240); arr.append([e[loud + int(t * 48)] - thud for t in T])
    print(f"deck {k}: peak {PEAK[-1]:.1f} dBFS; brightest contact 3-12k {top:.1f} dB, thud 5 ms full band {thud:.1f} dB; contacts re thud time, dB re brightest (ms:dB:width) " + "  ".join(f"{t:+.1f}:{d:.0f}:{w:.2f}" for t, d, w in ck))
C = [c for r in CK for c in r]; nl = [c for c in C if c[1] < -0.5]
print(f"\ncontacts {len(C)} ({len(C)/6:.1f}/placement); others at {np.percentile([c[0] for c in nl],10):.1f}..{np.percentile([c[0] for c in nl],90):.1f} ms, level median {np.median([c[1] for c in nl]):.1f} dB; widths median {np.median([c[2] for c in C]):.2f} ms")
cm = np.mean(CS, 0); bm = np.mean(BS, 0); np.save("an/deck_contact_spec.npy", cm - cm.max()); np.save("an/deck_bed_spec.npy", bm - bm.max())
print("Hz       " + " ".join(f"{c:6.0f}" for c in cs[::3])); print("contact  " + " ".join(f"{v:6.1f}" for v in (cm - cm.max())[::3])); print("bed      " + " ".join(f"{v:6.1f}" for v in (bm - bm.max())[::3]))
print("\nbed (5 ms RMS) re the thud's full-band 5 ms peak, ms: " + " ".join(f"{t:6d}" for t in T)); print("100-1k:                     " + " ".join(f"{v:6.1f}" for v in np.median(BL, 0))); print("2-12k:                      " + " ".join(f"{v:6.1f}" for v in np.median(BH, 0)))
