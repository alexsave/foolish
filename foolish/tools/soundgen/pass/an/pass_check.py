"""The pass_an.py measures that measure.py lacks, on real vs a synth set: envelope from onset, snags, weave modulation."""
import sys, glob, wave, numpy as np
SR = 48000
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
def env(y, w): return 20 * np.log10(np.sqrt(np.convolve(y ** 2, np.ones(w) / w, "same")) + 1e-12)
T = [0, 40, 80, 120, 160, 200, 240, 280, 330]
def m(p):
    x = bp(load(p), 150, 15000); hi = env(bp(x, 1000, 15000), 240)[::48]; P = hi.max(); pk = int(np.argmax(hi))
    on = next(k for k in range(pk, 0, -1) if hi[k] < P - 20)
    e = [hi[on + t] - P if on + t < len(hi) else np.nan for t in T]; dur = next((k for k in range(pk, len(hi) - 15) if all(hi[k:k + 15] < P - 20)), len(hi)) - on
    O = on * 48; a = O + int(0.04 * SR); b = O + int(0.24 * SR); e2 = np.sqrt(np.convolve(bp(x, 1000, 15000) ** 2, np.ones(48) / 48, "same"))[a:b:24]
    sm = np.convolve(e2, np.ones(41) / 41, "same"); r = e2 / (sm + 1e-12) - 1; M = np.abs(np.fft.rfft(r * np.hanning(len(r)), 1024)) ** 2; fm = np.fft.rfftfreq(1024, 1 / 2000)
    band = (fm > 10) & (fm < 400); Mn = M[band] / M[band].mean(); fb = fm[band]
    w1 = Mn[(fb > 52) & (fb < 62)].max(); w2 = Mn[(fb > 91) & (fb < 101)].max()
    h = env(bp(x, 3000, 15000), 12); sn = 0; i = O
    while i < min(len(h) - 240, O + int(0.4 * SR)):
        if h[i] > np.median(h[i - 240:i + 240]) + 8 and env(bp(x, 1000, 15000), 240)[i] > P - 20 and h[i] == h[i - 24:i + 25].max(): sn += 1; i += 24
        i += 1
    return e, dur, w1, w2, sn
for tag, pat in (("real", sys.argv[1]), ("synth", sys.argv[2])):
    R = [m(p) for p in sorted(glob.glob(pat))]
    print(f"{tag:5s} env 0..330 ms: " + " ".join(f"{v:6.1f}" for v in np.nanmedian([r[0] for r in R], 0)) + f" | dur {np.median([r[1] for r in R]):.0f} ms ({min(r[1] for r in R)}-{max(r[1] for r in R)}) | weave 57 Hz x{np.median([r[2] for r in R]):.1f} 96 Hz x{np.median([r[3] for r in R]):.1f} | snags {np.median([r[4] for r in R]):.1f}")
