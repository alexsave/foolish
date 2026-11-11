"""Pitch summary per clip: the strongest resonance in 0.4-1.6 kHz and in 1.6-4.5 kHz at the start, middle and
end of the active part (LPC, 5 ms hop), and the 1-10 kHz centroid at the same three points."""
import sys, wave, numpy as np
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def lpc(fr, order):
    r = np.correlate(fr, fr, "full")[len(fr) - 1: len(fr) + order]; r[0] *= 1.0001; a = np.zeros(order + 1); a[0] = 1; e = r[0]
    for i in range(1, order + 1):
        k = -(r[i] + np.dot(a[1:i], r[i - 1:0:-1])) / e; a[1:i + 1] = a[1:i + 1] + k * a[i - 1::-1][:i]; e *= 1 - k * k
    return a
SR = 24000; hop = 120; win = 600
def track(p):
    x = load(p); x = np.convolve(x, np.ones(2) / 2, "same")[::2]; x = np.append(x[0], x[1:] - 0.9 * x[:-1])
    e = np.sqrt(np.convolve(x ** 2, np.ones(24) / 24, "same")); act = np.where(e > e.max() * 0.15)[0]; a0, a1 = act[0], act[-1]
    rows = []
    for s in range(a0, a1 - win, hop):
        fr = x[s:s + win] * np.hamming(win); a = lpc(fr, 20); H = 1 / np.abs(np.fft.rfft(a, 2048)); f = np.fft.rfftfreq(2048, 1 / SR)
        lo = (f > 400) & (f < 1600); hi = (f > 1600) & (f < 4500); m = (f > 1000) & (f < 10000)
        rows.append((f[lo][np.argmax(H[lo])], f[hi][np.argmax(H[hi])], (f[m] * H[m] ** 2).sum() / (H[m] ** 2).sum()))
    r = np.array(rows); k = len(r); pts = [r[: max(1, k // 4)].mean(0), r[k // 2 - k // 8: k // 2 + k // 8 + 1].mean(0), r[-max(1, k // 4):].mean(0)]
    return np.array(pts)
groups = {"real": [f"parts/real_thumb_{i}.wav" for i in range(1, 6)], "thumb4": [f"synth/thumb4_{v:02d}.wav" for v in range(1, 9)], "thumb5": [f"synth/thumb5_{v:02d}.wav" for v in range(1, 9)], "thumb6": [f"synth/thumb6_{v:02d}.wav" for v in range(1, 9)], "thumb8": [f"synth/thumb8_{v:02d}.wav" for v in range(1, 9)], "thumb9": [f"synth/thumb9_{v:02d}.wav" for v in range(1, 9)]}
for g, files in groups.items():
    P = np.median([track(p) for p in files], axis=0)
    print(f"{g:7s} low res start/mid/end {P[0,0]:5.0f} {P[1,0]:5.0f} {P[2,0]:5.0f} Hz | high res {P[0,1]:5.0f} {P[1,1]:5.0f} {P[2,1]:5.0f} Hz | centroid {P[0,2]:5.0f} {P[1,2]:5.0f} {P[2,2]:5.0f} Hz")
