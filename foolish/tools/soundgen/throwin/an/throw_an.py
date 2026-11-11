"""Throw-in reps 2-8 (IMG_2933, cut -80..+400 ms around the peak): the strikes (3-15 kHz 0.25 ms envelope spikes >= 10 dB
over the local median), each listed per rep; the strike spectrum (1.5 ms windows) vs the bed (everything >= 1 ms from a
strike); the bed's low (150-1k) and high (2-15k) level over time re the loudest strike."""
import wave, numpy as np
SR = 48000
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
cs = np.load("an/cs40.npy")
def octs(y):
    n = 4096; Y = np.abs(np.fft.rfft(np.pad(y, (0, max(0, n - len(y))))[:n] * np.hanning(n))) ** 2; f = np.fft.rfftfreq(n, 1 / SR)
    return np.array([10 * np.log10(Y[(f >= c * 2 ** (-1 / 12)) & (f < c * 2 ** (1 / 12))].mean() + 1e-20) for c in cs])
T = [-20, -8, -3, 0, 3, 6, 10, 15, 20, 30, 40, 60, 80, 100, 130, 170, 220, 280]
allc, cspec, bspec, BL, BH, lvl = [], [], [], [], [], []
for r in range(2, 9):
    x = bp(load(f"parts/real_{r}.wav"), 150, 15000); lvl.append(20 * np.log10(np.abs(x).max()))
    h = 20 * np.log10(np.sqrt(np.convolve(bp(x, 3000, 15000) ** 2, np.ones(12) / 12, "same")) + 1e-12)
    loud = int(np.argmax(h)); top = h[loud]; ck = []; i = 0
    while i < len(h):
        ctx = np.median(h[max(0, i - 240): i + 240])
        if h[i] > ctx + 10 and h[i] > top - 30 and h[i] == h[max(0, i - 24): i + 25].max():
            wid = sum(1 for k in range(max(0, i - 96), min(len(h), i + 96)) if h[k] > h[i] - 10) / 48
            ck.append(((i - loud) / 48, h[i] - top, wid)); i += 24
        i += 1
    print(f"rep {r} peak {lvl[-1]:6.1f} dBFS, {len(ck)} strikes (ms : dB : width): " + "  ".join(f"{t:+6.1f}:{d:5.1f}:{w:.2f}" for t, d, w in ck))
    allc += [c for c in ck if abs(c[0]) < 60]
    mask = np.ones(len(x), bool)
    for t, d, w in ck:
        c = loud + int(t * 48); cspec.append(octs(x[max(0, c - 24): c + 48]) - top / 1); mask[max(0, c - 48): c + 120] = False
    bspec.append(octs(np.where(mask, x, 0)[loud - 480: loud + 9600]))
    for arr, lo, hi in ((BL, 150, 1000), (BH, 2000, 15000)):
        b = np.where(mask, bp(x, lo, hi), 0); e = 20 * np.log10(np.sqrt(np.convolve(b ** 2, np.ones(240) / 240, "same")) + 1e-12)
        arr.append([e[loud + t * 48] - top if 0 <= loud + t * 48 < len(e) else np.nan for t in T])
C = np.array(allc); nl = C[C[:, 1] < -0.5]
print(f"\nstrikes within 60 ms: {len(C)} ({len(C)/7:.1f}/rep); non-loudest at {np.percentile(nl[:,0],10):.1f}..{np.percentile(nl[:,0],90):.1f} ms, level median {np.median(nl[:,1]):.1f} dB (10-90%: {np.percentile(nl[:,1],10):.1f}..{np.percentile(nl[:,1],90):.1f}); widths median {np.median(C[:,2]):.2f} ms")
cm = np.mean(cspec, 0); bm = np.mean(bspec, 0)
print("Hz          " + " ".join(f"{c:6.0f}" for c in cs[::4]))
print("strike spec " + " ".join(f"{v:6.1f}" for v in (cm - cm.max())[::4]))
print("bed spec    " + " ".join(f"{v:6.1f}" for v in (bm - bm.max())[::4]))
print("\nbed re loudest strike, ms: " + " ".join(f"{t:6d}" for t in T))
print("bed 150-1k:                " + " ".join(f"{v:6.1f}" for v in np.nanmedian(BL, 0)))
print("bed 2-15k:                 " + " ".join(f"{v:6.1f}" for v in np.nanmedian(BH, 0)))
np.save("an/tin_strike_spec.npy", cm - cm.max()); np.save("an/tin_bed_spec.npy", bm - bm.max())
