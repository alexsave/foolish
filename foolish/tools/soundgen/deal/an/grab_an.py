"""Grabs 2-5 (IMG_2942): envelope curves on the peak (1 ms RMS, 0.5 ms hop, 150-15k), 1/12-octave spectra of the
first 60 ms, LPC peaks over time, grain. Prints the tables the synth uses."""
import wave, numpy as np
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
SR = 48000
def bandpass(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
T = np.array([-10, -5, -3, -2, -1, 0, 1, 2, 4, 6, 8, 12, 16, 20, 30, 40, 60, 80, 100, 130])   # ms re peak
cs = np.geomspace(150, 14000, 60)
env = {}; spec = {}
for g in (2, 3, 4, 5):
    x = bandpass(load(f"parts/real_grab_{g}.wav"), 150, 15000); a = np.sqrt(np.convolve(x ** 2, np.ones(48) / 48, "same")); pk = int(np.argmax(a))
    d = 20 * np.log10(a + 1e-12) - 20 * np.log10(a[pk]); env[g] = np.array([d[pk + int(t * 48)] if 0 <= pk + int(t * 48) < len(d) else np.nan for t in T])
    seg = x[max(0, pk - 240): pk + 2880]; n = 8192; S = np.abs(np.fft.rfft(np.pad(seg, (0, n - len(seg))) * np.hanning(n))) ** 2; f = np.fft.rfftfreq(n, 1 / SR)
    v = np.array([10 * np.log10(S[(f >= c * 2 ** (-1 / 24)) & (f < c * 2 ** (1 / 24))].mean() + 1e-20) for c in cs]); spec[g] = v - v.max()
    # grain over the first 40 ms
    y = x[pk: pk + 1920]; kurt = np.mean(y ** 4) / np.mean(y ** 2) ** 2
    Sx = [np.abs(np.fft.rfft(y[i:i + 512] * np.hanning(512))) ** 2 for i in range(0, len(y) - 512, 128)]; ff = np.fft.rfftfreq(512, 1 / SR); k = (ff > 700) & (ff < 6000)
    flat = np.median([np.exp(np.mean(np.log(s[k] + 1e-20))) / np.mean(s[k]) for s in Sx])
    print(f"grab {g}: kurtosis(0-40 ms) {kurt:5.1f}  flatness {flat:.2f}")
print("\nenvelope ms:    " + " ".join(f"{t:6d}" for t in T))
for g in env: print(f"grab {g} dB:      " + " ".join(f"{v:6.1f}" for v in env[g]))
hard = np.nanmean([env[2], env[3]], 0); soft = np.nanmean([env[4], env[5]], 0)
print("HARD_ENV = [" + ", ".join(f"{v:.1f}" for v in hard) + "]"); print("SOFT_ENV = [" + ", ".join(f"{v:.1f}" for v in soft) + "]")
np.save("an/grab_spec_hard.npy", np.mean([spec[2], spec[3]], 0)); np.save("an/grab_spec_soft.npy", np.mean([spec[4], spec[5]], 0)); np.save("an/grab_cs.npy", cs)
for nm, s in (("hard", np.mean([spec[2], spec[3]], 0)), ("soft", np.mean([spec[4], spec[5]], 0))):
    print(f"{nm} spectrum @ 250/500/1k/2k/4k/8k/12k: " + " ".join(f"{np.interp(np.log(c), np.log(cs), s):5.1f}" for c in (250, 500, 1000, 2000, 4000, 8000, 12000)))
