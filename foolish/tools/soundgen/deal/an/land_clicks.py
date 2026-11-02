"""Clicks and bed of grabs 2-5, measured separately. Clicks: spikes in the 3-15 kHz envelope (0.25 ms RMS) that rise
>= 10 dB above the median of the 10 ms around them; their times re the loudest, levels re the loudest, -10 dB widths.
Click spectrum: 1.5 ms windows on each click. Bed: everything at least 1 ms from any click, its spectrum and its
low/high envelope over time."""
import wave, numpy as np
SR = 48000
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
cs = np.geomspace(150, 14000, 40)
def octs(y):
    n = 4096; Y = np.abs(np.fft.rfft(np.pad(y, (0, max(0, n - len(y))))[:n] * np.hanning(n))) ** 2; f = np.fft.rfftfreq(n, 1 / SR)
    v = np.array([10 * np.log10(Y[(f >= c * 2 ** (-1 / 12)) & (f < c * 2 ** (1 / 12))].mean() + 1e-20) for c in cs]); return v
allc, cspec, bspec, bedlo, bedhi = [], [], [], [], []
for g in (4, 1, 2, 3, 5):
    x = bp(load(f"parts/real_land_{g}.wav"), 150, 15000); hi = bp(x, 3000, 15000); w = 12
    e = 20 * np.log10(np.sqrt(np.convolve(hi ** 2, np.ones(w) / w, "same")) + 1e-12)
    loud = int(np.argmax(e)); top = e[loud]; ck = []
    i = 0
    while i < len(e):
        lo, hh = max(0, i - 240), min(len(e), i + 240); ctx = np.median(e[lo:hh])
        if e[i] > ctx + 10 and e[i] > top - 30 and e[i] == e[max(0, i - 24): i + 25].max():
            wid = sum(1 for k in range(max(0, i - 96), min(len(e), i + 96)) if e[k] > e[i] - 10) / 48
            ck.append(((i - loud) / 48, e[i] - top, wid)); i += 24
        i += 1
    print(f"landing {g}: {len(ck)} clicks (ms re loudest : dB : -10 dB width ms): " + "  ".join(f"{t:+6.1f}:{d:5.1f}:{wd:.2f}" for t, d, wd in ck))
    allc += ck
    mask = np.ones(len(x), bool)
    for t, d, wd in ck:
        c = loud + int(t * 48); cspec.append(octs(x[max(0, c - 24): c + 48])); mask[max(0, c - 48): c + 96] = False
    seg = x[loud - 480: loud + 6000]; m = mask[loud - 480: loud + 6000]; bspec.append(octs(np.where(m, seg, 0)))
    for arr, lo_, hi_ in ((bedlo, 150, 1000), (bedhi, 2000, 15000)):
        b = np.where(mask, bp(x, lo_, hi_), 0); ee = 20 * np.log10(np.sqrt(np.convolve(b ** 2, np.ones(240) / 240, "same")) + 1e-12)
        arr.append([ee[loud + int(t * 48)] - (top) for t in (-8, -3, 3, 8, 15, 25, 40, 60, 80, 100)])
C = np.array(allc); print(f"\nall clicks: {len(C)}; per grab {len(C)/4:.1f}; times re loudest {np.percentile(C[:,0],10):.1f}..{np.percentile(C[:,0],90):.1f} ms; levels of the non-loudest median {np.median(C[C[:,1]<-0.5,1]):.1f} dB; widths median {np.median(C[:,2]):.2f} ms")
cm = np.mean(cspec, 0); bm = np.mean(bspec, 0); ref = max(cm.max(), bm.max())
print("octave:        " + " ".join(f"{c:6.0f}" for c in cs[::4]))
print("click spectrum " + " ".join(f"{v:6.1f}" for v in (cm - cm.max())[::4]))
print("bed spectrum   " + " ".join(f"{v:6.1f}" for v in (bm - bm.max())[::4]))
print("\nbed level re loudest click (dB), ms:      -8     -3      3      8     15     25     40     60     80    100")
print("bed 150-1k Hz:                      " + " ".join(f"{v:6.1f}" for v in np.median(bedlo, 0)))
print("bed 2-15 kHz:                       " + " ".join(f"{v:6.1f}" for v in np.median(bedhi, 0)))
np.save("an/land_click_spec_all.npy", cm - cm.max()); np.save("an/land_bed_spec_all.npy", bm - bm.max()); np.save("an/cs40.npy", cs)
