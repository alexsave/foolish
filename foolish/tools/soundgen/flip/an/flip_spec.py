"""Spectra for the flip model: the slide (whole slide cut), the snap click (1.5 ms windows on reps with a click),
the whup pulses (20 ms windows on each), and the slide's moving resonances (LPC order 20 at 24 kHz, 5 ms hop)."""
import wave, numpy as np
SR = 48000; S0 = int(0.35 * SR)
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
cs = np.load("an/cs40.npy")
def octs(y, n=4096):
    Y = np.abs(np.fft.rfft(np.pad(y, (0, max(0, n - len(y))))[:n] * np.hanning(n))) ** 2; f = np.fft.rfftfreq(n, 1 / SR)
    return np.array([10 * np.log10(Y[(f >= c * 2 ** (-1 / 12)) & (f < c * 2 ** (1 / 12))].mean() + 1e-20) for c in cs])
def lpc(fr, order):
    r = np.correlate(fr, fr, "full")[len(fr) - 1: len(fr) + order]; r[0] *= 1.0001; a = np.zeros(order + 1); a[0] = 1; e = r[0]
    for i in range(1, order + 1):
        k = -(r[i] + np.dot(a[1:i], r[i - 1:0:-1])) / e; a[1:i + 1] = a[1:i + 1] + k * a[i - 1::-1][:i]; e *= 1 - k * k
    return a
sl = []; sn = []; wh = []; tracks = []
for k in range(1, 8):
    x = bp(load(f"parts/flip_{k}.wav"), 100, 15000); s = bp(load(f"parts/fslide_{k}.wav"), 100, 15000)
    sl.append(octs(s, 8192) - 10 * np.log10(np.mean(s ** 2) + 1e-20))
    if k in (3, 4, 5, 7): sn.append(octs(x[S0 - 24: S0 + 48]))
    w = bp(load(f"parts/fwhup_{k}.wav"), 100, 15000); e = np.sqrt(np.convolve(bp(w, 150, 800) ** 2, np.ones(240) / 240, "same")); j = int(np.argmax(e))
    wh.append(octs(w[max(0, j - 480): j + 480]))
    # LPC resonances over the slide: strongest pole in 1-4 kHz and 4-9 kHz at 5 points
    y = np.convolve(s, np.ones(2) / 2, "same")[::2]; y = np.append(y[0], y[1:] - 0.9 * y[:-1]); n = len(y); row = []
    for q in np.linspace(0.25, 0.92, 5):
        c = int(q * n); fr = y[max(0, c - 300): c + 300] * np.hanning(len(y[max(0, c - 300): c + 300])); a = lpc(fr, 20); r = np.roots(a)
        r = r[np.imag(r) > 0]; fq = np.angle(r) * 24000 / (2 * np.pi); bw = -np.log(np.abs(r)) * 24000 / np.pi
        out = []
        for lo, hi in ((1000, 4000), (4000, 9000)):
            m = (fq > lo) & (fq < hi) & (bw < 1500)
            out.append(fq[m][np.argmin(bw[m])] if m.any() else np.nan)
        row.append(out)
    tracks.append(row)
for name, arr in (("slide", sl), ("snap", sn), ("whup", wh)):
    m = np.mean(arr, 0); m -= m.max(); np.save(f"an/flip_{name}_spec.npy", m)
    print(f"{name:6s}" + " ".join(f"{v:6.1f}" for v in m[::3]))
print("Hz    " + " ".join(f"{c:6.0f}" for c in cs[::3]))
T = np.array(tracks)
print("\nslide resonances at 25..92% of the slide (median over reps), 1-4 kHz: " + " ".join(f"{v:.0f}" for v in np.nanmedian(T[:, :, 0], 0)) + "   4-9 kHz: " + " ".join(f"{v:.0f}" for v in np.nanmedian(T[:, :, 1], 0)))
for k, row in enumerate(tracks, 1): print(f"  rep {k}: " + "  ".join(f"{a:.0f}/{b:.0f}" for a, b in row))
