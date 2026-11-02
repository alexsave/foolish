import sys, wave, glob, numpy as np
SR = 48000; pat = sys.argv[1]
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
T = [5, 10, 20, 30, 40, 60, 80, 100, 130, 160]
def curves(p):
    x = load(p); full = bp(x, 150, 15000); pk = int(np.argmax(np.abs(full))); out = []
    for lo, hi in ((150, 1000), (2000, 15000)):
        b = bp(x, lo, hi); e = 20 * np.log10(np.sqrt(np.convolve(b ** 2, np.ones(240) / 240, "same")) + 1e-12); ref = e[max(0, pk - 48): pk + 96].max()
        out.append([e[pk + t * 48] - ref if pk + t * 48 < len(e) else np.nan for t in T])
    return np.array(out)
print("ms after main click:       " + " ".join(f"{t:6d}" for t in T))
for g, p in (("real 4", ["parts/real_land_4.wav"]), ("synth", sorted(glob.glob(pat)))):
    C = np.nanmedian([curves(q) for q in p], 0)
    print(f"{g:9s} lows 150-1k (dB):  " + " ".join(f"{v:6.1f}" for v in C[0])); print(f"{g:9s} highs 2-15k (dB): " + " ".join(f"{v:6.1f}" for v in C[1]))
