# zoom.py wav t0 t1 fmax out.png [title]: a close spectrogram of one stretch (drawing aid)
import sys, wave, numpy as np
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
fn, t0, t1, fmax, out = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
w = wave.open(fn); sr = w.getframerate(); w.setpos(int(t0 * sr)); n = int((t1 - t0) * sr)
a = np.frombuffer(w.readframes(n), dtype=np.uint8).reshape(-1, 6)
def ch(o):
    x = (a[:, o].astype(np.int32) | (a[:, o + 1].astype(np.int32) << 8) | (a[:, o + 2].astype(np.int32) << 16)); return np.where(x >= 1 << 23, x - (1 << 24), x) / 8388608.0
x = (ch(0) + ch(3)) / 2
fig, ax = plt.subplots(2, 1, figsize=(16, 9), gridspec_kw={"height_ratios": [3, 1]}, sharex=True)
ax[0].specgram(x, NFFT=4096, Fs=sr, noverlap=4096 - 256, cmap="magma", vmin=-140, xextent=(t0, t1))
ax[0].set_ylim(0, fmax); ax[0].set_title(sys.argv[6] if len(sys.argv) > 6 else fn)
t = np.arange(len(x)) / sr + t0; ax[1].plot(t, x, lw=0.3); ax[1].set_xlim(t0, t1)
plt.tight_layout(); plt.savefig(out, dpi=80)
