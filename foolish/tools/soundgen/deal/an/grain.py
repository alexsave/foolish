import wave, glob, numpy as np
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768, w.getframerate()
def bandpass(x, sr, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / sr); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
def g(p, lo, hi):
    x, sr = load(p); y = bandpass(x, sr, lo, hi)
    e10 = np.sqrt(np.convolve(y ** 2, np.ones(480) / 480, "same")); act = e10 > e10.max() * 0.3; ya = y[act]
    kurt = np.mean(ya ** 4) / np.mean(ya ** 2) ** 2
    crest = 20 * np.log10(np.abs(ya).max() / np.sqrt(np.mean(ya ** 2)))
    e = np.sqrt(np.convolve(y ** 2, np.ones(12) / 12, "same")); s = np.sqrt(np.convolve(y ** 2, np.ones(96) / 96, "same"))
    micro = np.std(20 * np.log10((e[act] + 1e-12) / (s[act] + 1e-12)))
    return kurt, crest, micro
for lo, hi in ((4000, 12000), (1000, 4000)):
    print(f"band {lo}-{hi} Hz:")
    for grp, pat in (("real", "parts/real_thumb_*.wav"), ("thumb8", "synth/thumb8_0*.wav")):
        R = np.array([g(p, lo, hi) for p in sorted(glob.glob(pat))])
        print(f"  {grp:7s} kurtosis {np.median(R[:,0]):4.2f}  crest {np.median(R[:,1]):5.1f} dB  micro-roughness (0.25 vs 2 ms) {np.median(R[:,2]):4.2f} dB")
