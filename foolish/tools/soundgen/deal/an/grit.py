import sys, wave, glob, numpy as np
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768, w.getframerate()
def m(p):
    x, sr = load(p); h = int(0.001 * sr); e = np.sqrt(np.convolve(x ** 2, np.ones(h) / h, "same")); a = np.where(e > e.max() * 0.2)[0]; y = x[a[0]:a[-1]]
    fl = []
    for i in range(0, len(y) - 1024, 256):
        S = np.abs(np.fft.rfft(y[i:i + 1024] * np.hanning(1024))) ** 2; f = np.fft.rfftfreq(1024, 1 / sr); k = (f > 700) & (f < 6000)
        fl.append(np.exp(np.mean(np.log(S[k] + 1e-20))) / np.mean(S[k]))
    X = np.fft.rfft(y); f = np.fft.rfftfreq(len(y), 1 / sr); X[(f < 2000) | (f > 15000)] = 0; yh = np.fft.irfft(X, len(y))
    eh = np.sqrt(np.convolve(yh ** 2, np.ones(h) / h, "same")); sm = np.convolve(eh, np.ones(20 * h) / (20 * h), "same"); act = eh > eh.max() * 0.2
    return np.median(fl), np.std(20 * np.log10((eh[act] + 1e-9) / (sm[act] + 1e-9)))
for g, pat in (("real", "parts/real_thumb_*.wav"), ("thumb6", "synth/thumb9_0*.wav")):
    r = np.array([m(p) for p in sorted(glob.glob(pat))]); print(f"{g:7s} flatness 0.7-6 kHz {np.median(r[:,0]):.3f} (lower = more tonal)  roughness {np.median(r[:,1]):.2f} dB")
