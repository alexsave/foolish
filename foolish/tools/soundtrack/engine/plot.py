# plot.py: draws what anal measured (no measuring here). usage: plot.py prefix title [sections] [seam]
import sys, numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

pre, title = sys.argv[1], sys.argv[2]
secs = [float(x) for x in sys.argv[3].split(",")] if len(sys.argv) > 3 and sys.argv[3] else []
with open(pre + ".spec", "rb") as f:
    nfr, nb, hop, win = np.frombuffer(f.read(16), dtype=np.int32)
    S = np.frombuffer(f.read(), dtype=np.float32).reshape(nfr, nb)
loud = np.loadtxt(pre + ".loud")
ch = np.loadtxt(pre + ".chroma")
ons = np.loadtxt(pre + ".ons") if open(pre + ".ons").read().strip() else np.zeros((0, 3))
if ons.ndim == 1: ons = ons.reshape(1, -1)
dur = (nfr * hop + win) / 48000.0
fig, ax = plt.subplots(4, 1, figsize=(16, 13), sharex=True, gridspec_kw={"height_ratios": [3, 1.2, 1.4, 0.8]})
flo, fhi = 25, 20000
ext = [win / 2 / 48000, (nfr * hop + win / 2) / 48000, 0, nb]
top = S.max()
ax[0].imshow(S.T, origin="lower", aspect="auto", extent=ext, cmap="magma", vmin=top - 90, vmax=top)
ticks = [50, 100, 200, 500, 1000, 2000, 5000, 10000]
ax[0].set_yticks([nb * np.log(t / flo) / np.log(fhi / flo) for t in ticks]); ax[0].set_yticklabels([str(t) for t in ticks])
ax[0].set_ylabel("Hz"); ax[0].set_title(title + "  (spectrogram, 90 dB range)")
ax[1].plot(loud[:, 0], loud[:, 1], lw=0.5, color="0.6", label="momentary 0.4 s")
ax[1].plot(loud[:, 0], loud[:, 2], lw=1.5, color="C0", label="short-term 3 s")
ax[1].axhline(-16, color="C3", lw=0.8, ls="--", label="-16 target")
ax[1].set_ylim(-40, -5); ax[1].set_ylabel("LUFS"); ax[1].legend(loc="lower right", fontsize=8)
names = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"]
ax[2].imshow(ch[:, 1:].T, origin="lower", aspect="auto", extent=[ch[0, 0], ch[-1, 0], -0.5, 11.5], cmap="Greys", vmin=0, vmax=1)
ax[2].set_yticks(range(12)); ax[2].set_yticklabels(names, fontsize=7); ax[2].set_ylabel("chroma")
for b, c in zip(range(3), ["C3", "C2", "C0"]):
    o = ons[ons[:, 0] == b]
    if len(o): ax[3].vlines(o[:, 1], b, b + 0.8 * o[:, 2] / o[:, 2].max(), color=c, lw=0.6)
ax[3].set_yticks([0.4, 1.4, 2.4]); ax[3].set_yticklabels(["low", "mid", "high"]); ax[3].set_ylabel("onsets"); ax[3].set_xlabel("s")
for a in ax:
    for s in secs: a.axvline(s, color="cyan" if a is ax[0] else "C1", lw=0.8, alpha=0.7)
ax[3].set_xlim(0, dur)
plt.tight_layout(); plt.savefig(pre + ".png", dpi=80)
print("plot", pre + ".png")
