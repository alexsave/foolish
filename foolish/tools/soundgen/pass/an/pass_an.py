"""Pass slide ("shfff": two fingers sliding the card across the cloth), IMG_2936's 6 slides, parts/pass_k.wav with the
onset at 0.1 s. Envelope (1-15 kHz and 150-1k, 5 ms RMS, re its peak, on absolute time from onset), spectrum of the
body, LPC resonance track, the snags (3-15 kHz 0.25 ms spikes >= 8 dB over the local median) and the envelope's
modulation spectrum (is there a weave rate?)."""
import wave, numpy as np
SR = 48000; O = int(0.1 * SR)
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
def env(y, w): return 20 * np.log10(np.sqrt(np.convolve(y ** 2, np.ones(w) / w, "same")) + 1e-12)
cs = np.load("../an/cs40.npy")
def octs(y, n=8192):
    Y = np.zeros(n // 2 + 1); c = 0
    for i in range(0, len(y) - n + 1, n // 4): Y += np.abs(np.fft.rfft(y[i:i + n] * np.hanning(n))) ** 2; c += 1
    if c == 0: Y = np.abs(np.fft.rfft(np.pad(y * np.hanning(len(y)), (0, n - len(y))))) ** 2
    f = np.fft.rfftfreq(n, 1 / SR); return np.array([10 * np.log10(Y[(f >= q * 2 ** (-1 / 12)) & (f < q * 2 ** (1 / 12))].mean() + 1e-20) for q in cs])
def lpc(fr, order):
    r = np.correlate(fr, fr, "full")[len(fr) - 1: len(fr) + order]; r[0] *= 1.0001; a = np.zeros(order + 1); a[0] = 1; e = r[0]
    for i in range(1, order + 1):
        k = -(r[i] + np.dot(a[1:i], r[i - 1:0:-1])) / e; a[1:i + 1] = a[1:i + 1] + k * a[i - 1::-1][:i]; e *= 1 - k * k
    return a
T = [0, 20, 40, 60, 80, 100, 120, 140, 160, 180, 200, 220, 240, 260, 280, 300, 330, 360, 400]
EH, EL, SP, TR, SN, MS = [], [], [], [], [], []
for k in range(1, 7):
    x = bp(load(f"parts/pass_{k}.wav"), 150, 15000); hi = env(bp(x, 1000, 15000), 240); lo = env(bp(x, 150, 1000), 240); P = hi.max()
    EH.append([hi[O + t * 48] - P for t in T]); EL.append([lo[O + t * 48] - P for t in T])
    body = x[O + int(0.05 * SR): O + int(0.25 * SR)]; SP.append(octs(body, 4096) - 10 * np.log10(np.mean(body ** 2)))
    y = np.convolve(x, np.ones(2) / 2, "same")[::2]; y = np.append(y[0], y[1:] - 0.9 * y[:-1]); row = []
    for t in (40, 100, 160, 220, 260):
        c = (O + t * 48) // 2; fr = y[c - 300:c + 300] * np.hanning(600); a = lpc(fr, 20); rt = np.roots(a); rt = rt[np.imag(rt) > 0]
        fq = np.angle(rt) * 24000 / (2 * np.pi); bw = -np.log(np.abs(rt)) * 24000 / np.pi; m = (fq > 800) & (fq < 6000) & (bw < 1200)
        row.append(fq[m][np.argmin(bw[m])] if m.any() else np.nan)
    TR.append(row)
    h = env(bp(x, 3000, 15000), 12); ev = hi; sn = []; i = O
    while i < O + int(0.4 * SR):
        if h[i] > np.median(h[i - 240:i + 240]) + 8 and ev[i] > P - 20 and h[i] == h[i - 24:i + 25].max():
            sn.append(((i - O) / 48, h[i] - np.median(h[i - 240:i + 240]), sum(1 for j in range(i - 96, i + 96) if h[j] > h[i] - 10) / 48)); i += 24
        i += 1
    SN.append(sn)
    a = O + int(0.04 * SR); b = O + int(0.24 * SR); e2 = np.sqrt(np.convolve(bp(x, 1000, 15000) ** 2, np.ones(48) / 48, "same"))[a:b:24]   # 1 ms RMS at 2 kHz
    sm = np.convolve(e2, np.ones(41) / 41, "same"); r = e2 / (sm + 1e-12) - 1; M = np.abs(np.fft.rfft(r * np.hanning(len(r)), 1024)) ** 2; fm = np.fft.rfftfreq(1024, 1 / 2000)
    MS.append(M[(fm > 10) & (fm < 400)] / M[(fm > 10) & (fm < 400)].mean())
    print(f"pass {k}: snags (ms after onset : dB over local : width ms): " + "  ".join(f"{t:.0f}:{d:.0f}:{w:.2f}" for t, d, w in sn))
print("\nms from onset:  " + " ".join(f"{t:5d}" for t in T)); print("1-15k re peak:  " + " ".join(f"{v:5.1f}" for v in np.median(EH, 0))); print("150-1k re peak: " + " ".join(f"{v:5.1f}" for v in np.median(EL, 0)))
m = np.mean(SP, 0); m -= m.max(); np.save("pass_spec.npy", m); print("\nHz    " + " ".join(f"{c:6.0f}" for c in cs[::3])); print("body  " + " ".join(f"{v:6.1f}" for v in m[::3]))
TR = np.array(TR); print("\nLPC strongest 0.8-6 kHz resonance at 40/100/160/220/260 ms:", " ".join(f"{v:.0f}" for v in np.nanmedian(TR, 0)))
for k, r in enumerate(TR, 1): print(f"   pass {k}: " + " ".join(f"{v:.0f}" for v in r))
C = [c for r in SN for c in r]; print(f"\nsnags {len(C)/6:.1f}/slide, {len(C)/6/0.25:.0f}/s over the ~250 ms body; over local median {np.median([c[1] for c in C]):.1f} dB; width {np.median([c[2] for c in C]):.2f} ms")
fm = np.fft.rfftfreq(1024, 1 / 2000); fm = fm[(fm > 10) & (fm < 400)]; Mm = np.mean(MS, 0); top = np.argsort(Mm)[-6:][::-1]
print("modulation spectrum of the 1 ms envelope (40-240 ms), strongest rates (Hz : x mean): " + "  ".join(f"{fm[i]:.0f}:{Mm[i]:.1f}" for i in top))
