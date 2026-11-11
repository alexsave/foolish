"""LPC on Alex's slides: per 5 ms frame (25 ms window), an all-pole fit; its resonances (formants) over time,
plus the spectral centroid over time. Order 20 at 24 kHz (decimated: the slides end ~11 kHz)."""
import wave, numpy as np, matplotlib
matplotlib.use("Agg"); import matplotlib.pyplot as plt
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def lpc(fr, order):
    r = np.correlate(fr, fr, "full")[len(fr) - 1: len(fr) + order]; r[0] *= 1.0001
    a = np.zeros(order + 1); a[0] = 1; e = r[0]
    for i in range(1, order + 1):
        k = -(r[i] + np.dot(a[1:i], r[i - 1:0:-1])) / e; a[1:i + 1] = a[1:i + 1] + k * a[i - 1::-1][:i]; e *= 1 - k * k
    return a, e
SR = 24000; ORDER = 20; hop = int(0.005 * SR); win = int(0.025 * SR)
fig, ax = plt.subplots(2, 5, figsize=(22, 8))
for col, i in enumerate(range(1, 6)):
    x = load(f"parts/real_thumb_{i}.wav"); x = np.convolve(x, np.ones(2) / 2, "same")[::2]           # 48 -> 24 kHz
    x = np.append(x[0], x[1:] - 0.9 * x[:-1])                                                      # pre-emphasis
    h = 24; e = np.sqrt(np.convolve(x ** 2, np.ones(h) / h, "same")); act = e > e.max() * 0.15
    T, F, B, cent = [], [], [], []
    for s in range(0, len(x) - win, hop):
        if not act[s + win // 2]: continue
        fr = x[s:s + win] * np.hamming(win); a, err = lpc(fr, ORDER)
        roots = [r for r in np.roots(a) if np.imag(r) > 0]
        for r in roots:
            f = np.angle(r) * SR / (2 * np.pi); bw = -np.log(np.abs(r)) * SR / np.pi
            if 200 < f < 11500 and bw < 1200: T.append((s + win / 2) / SR * 1000); F.append(f); B.append(bw)
        S = np.abs(np.fft.rfft(fr, 1024)) ** 2; ff = np.fft.rfftfreq(1024, 1 / SR); cent.append(((s + win / 2) / SR * 1000, (ff * S).sum() / S.sum()))
    T, F, B = map(np.array, (T, F, B))
    ax[0, col].scatter(T, F, c=np.clip(B, 0, 1200), cmap="viridis_r", s=10); ax[0, col].set_yscale("log"); ax[0, col].set_ylim(200, 12000)
    ax[0, col].set_title(f"real_thumb_{i}: LPC resonances (dark = sharp)"); ax[0, col].set_xlabel("ms")
    c = np.array(cent); ax[1, col].plot(c[:, 0], c[:, 1], marker="."); ax[1, col].set_title("spectral centroid over time"); ax[1, col].grid(alpha=.3)
    # rising? fit a line to the centroid and to each sharp-resonance track in 1-11 kHz
    k = np.polyfit(c[:, 0], c[:, 1], 1)[0]
    sharp = (B < 400) & (F > 1000); kk = np.polyfit(T[sharp], np.log2(F[sharp]), 1)[0] * 1000 if sharp.sum() > 5 else float("nan")
    print(f"real_thumb_{i}: active {c[0,0]:.0f}-{c[-1,0]:.0f} ms; centroid {c[0,1]:.0f} -> {c[-1,1]:.0f} Hz (slope {k*100:+.0f} Hz per 100 ms); sharp resonances >1 kHz drift {kk:+.2f} octaves/s")
fig.tight_layout(); fig.savefig("plots/thumb_lpc.png", dpi=65)
