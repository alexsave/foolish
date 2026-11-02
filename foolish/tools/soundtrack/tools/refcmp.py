"""Reference vs ours, whole-track production measures. Usage: refcmp.py OUT.png ref.wav ours.wav [more pairs...]
Per track: LUFS-ish (K-weighted RMS, 400 ms gated), short-term loudness spread, 1/3-oct long-term spectrum re its 1 kHz band,
low-end shares (sub <60, bass 60-250), stereo side/mid per band, crest (peak/RMS, 50 ms), onsets/s, tempo, spectral peaks.
Refs are 128k mp3: everything above ~16 kHz is codec, ignore it."""
import sys, wave, numpy as np, matplotlib
from scipy import signal
matplotlib.use("Agg"); import matplotlib.pyplot as plt
SR = 48000
def load(p):
    with wave.open(p) as w:
        n, ch, sw = w.getnframes(), w.getnchannels(), w.getsampwidth(); b = w.readframes(n)
    if sw == 3:
        a = np.frombuffer(b, np.uint8).reshape(-1, 3).astype(np.int32); x = (a[:, 0] | (a[:, 1] << 8) | (a[:, 2] << 16)); x = np.where(x >= 1 << 23, x - (1 << 24), x) / float(1 << 23)
    else:
        x = np.frombuffer(b, np.int16) / 32768.0
    return x.reshape(-1, ch).T.astype(np.float32)
def kweight(x):
    b1, a1 = signal.bilinear([1.53512485958697, -2.69169618940638, 1.19839281085285], [1, -1.69065929318241, 0.73248077421585], 1) if False else ([1.53512485958697, -2.69169618940638, 1.19839281085285], [1.0, -1.69065929318241, 0.73248077421585])
    b2, a2 = [1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621]
    return signal.lfilter(b2, a2, signal.lfilter(b1, a1, x))
cs = 1000 * 2 ** (np.arange(-17, 14) / 3)   # 1/3-oct 20 Hz .. 20 kHz
def measure(p):
    x = load(p); L, R = x[0], x[1] if len(x) > 1 else x[0]; M = (L + R) / 2; Sd = (L - R) / 2; m = {}
    kw = [kweight(c) for c in (L, R)]; blk = int(0.4 * SR); hop = blk // 4
    pw = np.array([sum(np.mean(k[i:i + blk] ** 2) for k in kw) for i in range(0, len(L) - blk, hop)]); lk = -0.691 + 10 * np.log10(pw + 1e-12)
    g = lk[lk > -70]; rel = 10 * np.log10(np.mean(10 ** (g / 10))) - 10; g2 = g[g > rel]
    m["LUFS"] = 10 * np.log10(np.mean(10 ** (g2 / 10)))
    st = np.convolve(10 ** (lk / 10), np.ones(12) / 12, "valid"); st = 10 * np.log10(st + 1e-12); st = st[st > m["LUFS"] - 20]
    m["LRA (10-95%)"] = np.percentile(st, 95) - np.percentile(st, 10)
    f, P = signal.welch(M, SR, nperseg=8192); fs, PS = signal.welch(Sd, SR, nperseg=8192)
    band = lambda Q, lo, hi: Q[(f >= lo) & (f < hi)].sum() + 1e-20
    tot = band(P, 20, 16000)
    m["sub <60 Hz share dB"] = 10 * np.log10(band(P, 20, 60) / tot); m["bass 60-250 share dB"] = 10 * np.log10(band(P, 60, 250) / tot)
    m["lowmid 250-800 dB"] = 10 * np.log10(band(P, 250, 800) / tot); m["mid 0.8-3k dB"] = 10 * np.log10(band(P, 800, 3000) / tot)
    m["presence 3-8k dB"] = 10 * np.log10(band(P, 3000, 8000) / tot); m["air 8-16k dB"] = 10 * np.log10(band(P, 8000, 16000) / tot)
    for lo, hi, t in ((20, 150, "low"), (150, 2000, "mid"), (2000, 16000, "high")):
        m[f"width S/M {t} dB"] = 10 * np.log10(band(PS, lo, hi) / band(P, lo, hi))
    w = int(0.05 * SR); k = len(M) // w; seg = M[:k * w].reshape(k, w); rms = np.sqrt((seg ** 2).mean(1)); pk = np.abs(seg).max(1); act = rms > rms.max() * 0.05
    m["crest 50ms dB"] = np.median(20 * np.log10(pk[act] / rms[act]))
    # onsets from spectral flux at 100 Hz frames
    ff, tt, Z = signal.stft(M, SR, nperseg=2048, noverlap=2048 - 480); A = np.log1p(100 * np.abs(Z)); flux = np.maximum(np.diff(A, axis=1), 0).sum(0)
    fl = flux - np.convolve(flux, np.ones(50) / 50, "same"); thr = np.std(fl) * 1.5
    on = [i for i in range(1, len(fl) - 1) if fl[i] > thr and fl[i] >= fl[i - 1] and fl[i] >= fl[i + 1]]; m["onsets/s"] = len(on) / (len(M) / SR)
    ac = np.correlate(fl[:6000] - fl[:6000].mean(), fl[:6000] - fl[:6000].mean(), "full")[5999:]; lags = np.arange(len(ac)); bpm = 6000 / np.maximum(lags, 1)
    ok = (bpm > 70) & (bpm < 180); m["tempo est BPM"] = bpm[ok][np.argmax(ac[ok])]
    # tonal richness: peaks in the long-term 0.1-5 kHz spectrum standing 6 dB over a 1/3-oct smooth
    Pd = 10 * np.log10(P + 1e-20); sm = np.convolve(Pd, np.ones(31) / 31, "same"); kk = np.where((f > 100) & (f < 5000))[0]
    m["tonal peaks 0.1-5k"] = sum(1 for i in kk if Pd[i] - sm[i] > 6 and Pd[i] == Pd[i - 3:i + 4].max())
    m["centroid Hz"] = float((f * P).sum() / P.sum())
    third = np.array([10 * np.log10(band(P, c * 2 ** (-1 / 6), c * 2 ** (1 / 6))) for c in cs]); third -= third[17]
    return m, third
out = sys.argv[1]; pairs = [(sys.argv[i], sys.argv[i + 1]) for i in range(2, len(sys.argv), 2)]
fig, axs = plt.subplots(len(pairs), 1, figsize=(11, 3.2 * len(pairs)), squeeze=False)
for j, (r, o) in enumerate(pairs):
    (mr, tr), (mo, to) = measure(r), measure(o)
    print(f"\n=== ref {r.split('/')[-1]}  vs  ours {o.split('/')[-2] if o.endswith('full.wav') else o.split('/')[-1]}")
    print(f"{'measure':24s} {'ref':>8s} {'ours':>8s} {'ours-ref':>9s}")
    for k in mr: print(f"{k:24s} {mr[k]:8.1f} {mo[k]:8.1f} {mo[k]-mr[k]:9.1f}")
    print("1/3-oct re 1 kHz, Hz: " + " ".join(f"{c:6.0f}" for c in cs[1::2]))
    print("ref                   " + " ".join(f"{v:6.1f}" for v in tr[1::2]))
    print("ours-ref              " + " ".join(f"{v:6.1f}" for v in (to - tr)[1::2]))
    a = axs[j, 0]; a.semilogx(cs, tr, label="ref " + r.split("/")[-1]); a.semilogx(cs, to, label="ours"); a.set_ylim(-40, 25); a.grid(alpha=.3); a.legend(fontsize=8); a.set_xlim(20, 16000)
fig.tight_layout(); fig.savefig(out, dpi=80)
