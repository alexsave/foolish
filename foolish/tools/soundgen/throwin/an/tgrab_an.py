"""Two-finger grab (IMG_2952, 8 grabs, parts/grab_k.wav, peak at 0.2 s): clicks (3-15 kHz 0.25 ms spikes >= 10 dB over
the local median), the contact spectrum (10 ms around the peak) and the low/high envelopes re the peak, each with the
room floor's level beside it so the event is told from the room."""
import wave, numpy as np
SR = 48000; P0 = int(0.2 * SR)
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
def env(y, w): return 20 * np.log10(np.sqrt(np.convolve(y ** 2, np.ones(w) / w, "same")) + 1e-12)
cs = np.load("an/cs40.npy")
def octs(y, n=8192):
    Y = np.abs(np.fft.rfft(np.pad(y * np.hanning(len(y)), (0, n - len(y))))) ** 2; f = np.fft.rfftfreq(n, 1 / SR)
    return np.array([10 * np.log10(Y[(f >= c * 2 ** (-1 / 12)) & (f < c * 2 ** (1 / 12))].mean() + 1e-20) for c in cs])
room = bp(load("parts/grab_room.wav"), 150, 15000)
T = [-30, -15, -8, -4, -2, 0, 2, 4, 8, 12, 20, 30, 50, 80]
CK, SP, EL, EH, PK = [], [], [], [], []
for k in range(1, 9):
    x = bp(load(f"parts/grab_{k}.wav"), 150, 15000); PK.append(20 * np.log10(np.abs(x).max()))
    h = env(bp(x, 3000, 15000), 12); top = h[P0 - 480:P0 + 480].max(); ck = []; i = P0 - 1440
    while i < P0 + 1440:
        if h[i] > np.median(h[i - 240:i + 240]) + 10 and h[i] > top - 25 and h[i] == h[i - 24:i + 25].max():
            ck.append(((i - P0) / 48, h[i] - top, sum(1 for j in range(i - 96, i + 96) if h[j] > h[i] - 10) / 48)); i += 24
        i += 1
    CK.append(ck); SP.append(octs(x[P0 - 240: P0 + 240]))
    full = env(x, 240)[P0 - 96:P0 + 96].max()
    EL.append([env(bp(x, 150, 1000), 240)[P0 + int(t * 48)] - full for t in T]); EH.append([env(bp(x, 2000, 15000), 240)[P0 + int(t * 48)] - full for t in T])
    print(f"grab {k}: peak {PK[-1]:.1f} dBFS; clicks (ms:dB:width) " + "  ".join(f"{t:+.1f}:{d:.0f}:{w:.2f}" for t, d, w in ck))
C = [c for r in CK for c in r]; print(f"\nclicks {len(C)/8:.1f}/grab; times {np.percentile([c[0] for c in C],10):.1f}..{np.percentile([c[0] for c in C],90):.1f} ms; widths median {np.median([c[2] for c in C]):.2f} ms")
m = np.mean(SP, 0); m -= m.max(); np.save("an/tgrab_spec.npy", m)

print("Hz      " + " ".join(f"{c:6.0f}" for c in cs[::3])); print("contact " + " ".join(f"{v:6.1f}" for v in m[::3]))
fr = env(room, 240).mean(); print(f"\nroom floor full-band 5 ms RMS: {fr:.1f} dBFS; grab peaks {np.median(PK):.1f} dBFS (median)")
print("ms re peak:     " + " ".join(f"{t:6d}" for t in T)); print("150-1k re full: " + " ".join(f"{v:6.1f}" for v in np.median(EL, 0))); print("2-15k re full:  " + " ".join(f"{v:6.1f}" for v in np.median(EH, 0)))
for b, lo, hi in (("150-1k", 150, 1000), ("2-15k", 2000, 15000)):
    print(f"room {b} 5 ms RMS: {env(bp(room, lo, hi), 240).mean():.1f} dBFS")
