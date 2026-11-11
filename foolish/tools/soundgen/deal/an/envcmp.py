import wave, glob, numpy as np, matplotlib
matplotlib.use("Agg"); import matplotlib.pyplot as plt
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768, w.getframerate()
def bandpass(x, sr, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / sr); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
def curve(p):
    x, sr = load(p); y = bandpass(x, sr, 1000, 15000); w, h = int(0.010 * sr), int(0.001 * sr)
    d = 20 * np.log10(np.sqrt(np.convolve(y ** 2, np.ones(w) / w, "same"))[::h] + 1e-12); pk = int(np.argmax(d)); d -= d[pk]
    out = np.full(400, np.nan); lo = pk - 150
    for k in range(400):
        if 0 <= lo + k < len(d): out[k] = d[lo + k]
    return out
t = np.arange(400) - 150
fig, ax = plt.subplots(figsize=(12, 5))
med = {}
for g, pat, col in (("real", "parts/real_thumb_*.wav", "tab:blue"), ("synth", "synth/thumb9_0[1-8].wav", "tab:orange")):
    C = np.array([curve(p) for p in sorted(glob.glob(pat))])
    for c in C: ax.plot(t, c, color=col, alpha=0.25, lw=0.8)
    med[g] = np.nanmedian(C, 0); ax.plot(t, med[g], color=col, lw=2.2, label=g + " median")
ax.set_xlim(-150, 250); ax.set_ylim(-40, 2); ax.grid(alpha=.3); ax.legend(); ax.set_xlabel("ms from peak"); ax.set_ylabel("dB re peak (1-15 kHz, 10 ms RMS)")
fig.tight_layout(); fig.savefig("plots/thumb_env_compare.png", dpi=80)
print("ms from peak:  " + " ".join(f"{k:5d}" for k in range(-120, 241, 20)))
for g in med: print(f"{g:5s} median:  " + " ".join(f"{med[g][k + 150]:5.1f}" for k in range(-120, 241, 20)))
