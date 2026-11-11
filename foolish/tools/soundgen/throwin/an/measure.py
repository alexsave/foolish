"""Every measure we have, on two sets of clips: real (A) vs synth (B). Usage: measure.py NAME 'globA' 'globB'
Prints a side-by-side table (median over each set, with the A range) and writes plots/NAME_cmp.png
(spectrograms of 3 of each, the 1-15 kHz envelope aligned on the peak, two-band envelopes, the 1/12-oct spectrum).
Everything is measured on 150 Hz-15 kHz (below 150 Hz the phone recordings are handling rumble)."""
import sys, glob, wave, numpy as np, matplotlib
matplotlib.use("Agg"); import matplotlib.pyplot as plt
SR = 48000
NAME, PA, PB = sys.argv[1], sys.argv[2], sys.argv[3]
cs = np.geomspace(150, 14000, 40)
def load(p):
    with wave.open(p) as w:
        x = np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
        assert w.getframerate() == SR
        return x
def bp(x, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
def rms_db(y, w):
    return 20 * np.log10(np.sqrt(np.convolve(y ** 2, np.ones(w) / w, "same")) + 1e-12)
def oct12(y):
    n = 8192; Y = np.zeros(n // 2 + 1); y = np.pad(y, (0, max(0, n - len(y)))); c = 0
    for i in range(0, len(y) - n + 1, n // 4): Y += np.abs(np.fft.rfft(y[i:i + n] * np.hanning(n))) ** 2; c += 1
    f = np.fft.rfftfreq(n, 1 / SR)
    return np.array([10 * np.log10(Y[(f >= q * 2 ** (-1 / 24)) & (f < q * 2 ** (1 / 24))].mean() / c + 1e-20) for q in cs])
T_ENV = np.arange(-60, 241)   # ms re peak
def measure(p):
    x = bp(load(p), 150, 15000); m = {}
    m["peak dBFS"] = 20 * np.log10(np.abs(x).max())
    pk0 = int(np.argmax(np.abs(x))); w10 = x[max(0, pk0 - 240): pk0 + 240]
    m["crest full 10ms dB"] = 20 * np.log10(np.abs(x).max() / np.sqrt(np.mean(w10 ** 2)))
    m["crest 150-1k 10ms dB"] = (lambda y: 20 * np.log10(np.abs(y[max(0, pk0 - 240): pk0 + 240]).max() / np.sqrt(np.mean(y[max(0, pk0 - 240): pk0 + 240] ** 2))))(bp(x, 150, 1000))
    m["peak lo/full dB"] = 20 * np.log10(np.abs(bp(x, 150, 1000)).max() / np.abs(x).max())
    a = 20 * np.log10(np.sqrt(np.mean(bp(x, 500, 10000) ** 2 * 1)) + 1e-12)
    hi = bp(x, 1000, 15000); e = rms_db(hi, 480)[::48]; pk = int(np.argmax(e)); e -= e[pk]
    env = np.array([e[pk + t] if 0 <= pk + t < len(e) else np.nan for t in T_ENV])
    act = np.where(e > -20)[0]; m["dur > -20 dB ms"] = act[-1] - act[0]
    act10 = np.where(e > -10)[0]; m["dur > -10 dB ms"] = act10[-1] - act10[0]
    m["rise -20->0 ms"] = pk - act[0]
    after = e[pk:]; m["fall to -10 ms"] = next((k for k, v in enumerate(after) if v < -10), np.nan)
    m["fall to -20 ms"] = next((k for k, v in enumerate(after) if v < -20), np.nan)
    pks = int(np.argmax(np.abs(x)))
    lo_b = rms_db(bp(x, 150, 1000), 240)[::48]; hi_b = rms_db(bp(x, 2000, 15000), 240)[::48]; ref = max(lo_b.max(), hi_b.max())
    ps = pks // 48
    two = [np.array([b[ps + t] - ref if 0 <= ps + t < len(b) else np.nan for t in T_ENV]) for b in (lo_b, hi_b)]
    m["low-high balance dB"] = lo_b.max() - hi_b.max()
    s = oct12(x[max(0, pks - 4800): pks + 9600]); sp = s - s.max()
    m["centroid Hz"] = float(np.sum(cs * 10 ** (s / 10)) / np.sum(10 ** (s / 10)))
    # clicks: 3-15 kHz 0.25 ms envelope spikes >= 10 dB over the local 10 ms median
    h = rms_db(bp(x, 3000, 15000), 12); top = h.max(); ck = []; i = 0
    while i < len(h):
        ctx = np.median(h[max(0, i - 240): i + 240])
        if h[i] > ctx + 10 and h[i] > top - 30 and h[i] == h[max(0, i - 24): i + 25].max():
            ck.append(((i - int(np.argmax(h))) / 48, h[i] - top, sum(1 for k in range(max(0, i - 96), min(len(h), i + 96)) if h[k] > h[i] - 10) / 48)); i += 24
        i += 1
    m["clicks"] = len(ck); m["click width ms"] = np.median([c[2] for c in ck]) if ck else np.nan
    m["click span ms"] = (max(c[0] for c in ck) - min(c[0] for c in ck)) if len(ck) > 1 else 0
    # texture, on the part within 10 dB of the peak (10 ms)
    for lo, hi_, tag in ((1000, 4000, "1-4k"), (4000, 12000, "4-12k")):
        y = bp(x, lo, hi_); e10 = np.sqrt(np.convolve(y ** 2, np.ones(480) / 480, "same")); ac = e10 > e10.max() * 0.3; ya = y[ac]
        m[f"kurtosis {tag}"] = np.mean(ya ** 4) / np.mean(ya ** 2) ** 2
        m[f"crest dB {tag}"] = 20 * np.log10(np.abs(ya).max() / np.sqrt(np.mean(ya ** 2)))
        ee = np.sqrt(np.convolve(y ** 2, np.ones(12) / 12, "same")); ss = np.sqrt(np.convolve(y ** 2, np.ones(96) / 96, "same"))
        m[f"micro-rough dB {tag}"] = np.std(20 * np.log10((ee[ac] + 1e-12) / (ss[ac] + 1e-12)))
    e1 = np.sqrt(np.convolve(hi ** 2, np.ones(48) / 48, "same")); e20 = np.sqrt(np.convolve(hi ** 2, np.ones(960) / 960, "same")); ac = e20 > e20.max() * 0.2
    m["rough 1/20ms dB"] = np.std(20 * np.log10((e1[ac] + 1e-12) / (e20[ac] + 1e-12)))
    fl = []
    seg = x[ac]
    for i in range(0, len(seg) - 1024, 256):
        S = np.abs(np.fft.rfft(seg[i:i + 1024] * np.hanning(1024))) ** 2; f = np.fft.rfftfreq(1024, 1 / SR); k = (f > 700) & (f < 6000)
        fl.append(np.exp(np.mean(np.log(S[k] + 1e-20))) / np.mean(S[k]))
    m["flatness 0.7-6k"] = np.median(fl) if fl else np.nan
    return m, env, two, sp, x, pks
A = sorted(glob.glob(PA)); B = sorted(glob.glob(PB))
RA = [measure(p) for p in A]; RB = [measure(p) for p in B]
keys = list(RA[0][0].keys())
print(f"{NAME}: A = {len(A)} ({PA}), B = {len(B)} ({PB})")
print(f"{'measure':24s} {'A median':>9s} {'A min':>8s} {'A max':>8s} {'B median':>9s}   flag")
for k in keys:
    va = np.array([r[0][k] for r in RA], float); vb = np.array([r[0][k] for r in RB], float)
    ma, mb = np.nanmedian(va), np.nanmedian(vb); lo, hi = np.nanmin(va), np.nanmax(va)
    flag = "  <-- outside A" if mb < lo or mb > hi else ""
    print(f"{k:24s} {ma:9.2f} {lo:8.2f} {hi:8.2f} {mb:9.2f}{flag}")
sa = np.median([r[3] for r in RA], 0); sb = np.median([r[3] for r in RB], 0)
print("\n1/12-oct spectrum, dB re own max, every 4th band (B - A):")
print("Hz     " + " ".join(f"{c:6.0f}" for c in cs[::4]))
print("A      " + " ".join(f"{v:6.1f}" for v in sa[::4]))
print("B-A    " + " ".join(f"{v:6.1f}" for v in (sb - sa)[::4]))
np.save(f"plots/{NAME}_specdiff.npy", sb - sa)
ea = np.nanmedian([r[1] for r in RA], 0); eb = np.nanmedian([r[1] for r in RB], 0)
pts = [-40, -20, -10, -5, 0, 5, 10, 20, 30, 50, 80, 120, 160, 200, 240]
print("\n1-15 kHz envelope (10 ms), dB re peak, ms:")
print("ms     " + " ".join(f"{t:6d}" for t in pts))
print("A      " + " ".join(f"{ea[t + 60]:6.1f}" for t in pts))
print("B-A    " + " ".join(f"{eb[t + 60] - ea[t + 60]:6.1f}" for t in pts))
for j, tag in ((0, "150-1k"), (1, "2-15k")):
    ta = np.nanmedian([r[2][j] for r in RA], 0); tb = np.nanmedian([r[2][j] for r in RB], 0)
    print(f"band {tag:7s} A " + " ".join(f"{ta[t + 60]:6.1f}" for t in pts))
    print(f"band {tag:7s} B-A" + " ".join(f"{tb[t + 60] - ta[t + 60]:6.1f}" for t in pts)[2:])
fig, ax = plt.subplots(3, 4, figsize=(22, 13)); fig.suptitle(f"{NAME}: real (top row) vs synth (middle row)")
for row, R in ((0, RA), (1, RB)):
    for c in range(3):
        if c >= len(R): continue
        x, pk = R[c][4], R[c][5]; seg = x[max(0, pk - 2880): pk + 11520]
        ax[row, c].specgram(seg + 1e-7 * np.random.randn(len(seg)), NFFT=512, Fs=SR, noverlap=448, cmap="magma", vmin=-150, vmax=-40)
        ax[row, c].set_ylim(0, 16000); ax[row, c].set_title(("real " if row == 0 else "synth ") + (A if row == 0 else B)[c].split("/")[-1])
    x, pk = R[0][4], R[0][5]; t = (np.arange(len(x)) - pk) / SR * 1000
    ax[row, 3].plot(t, x, lw=0.5); ax[row, 3].set_xlim(-60, 240); ax[row, 3].set_title("waveform, same scale"); ax[row, 3].set_ylim(-0.08, 0.08)
for R, col, lab in ((RA, "tab:blue", "real"), (RB, "tab:orange", "synth")):
    for k, r in enumerate(R):
        ax[2, 0].plot(T_ENV, r[1], color=col, alpha=0.5, label=lab if k == 0 else None)
        ax[2, 1].plot(T_ENV, r[2][0], color=col, alpha=0.5); ax[2, 2].plot(T_ENV, r[2][1], color=col, alpha=0.5)
        ax[2, 3].semilogx(cs, r[3], color=col, alpha=0.5)
ax[2, 0].set_title("1-15 kHz env re peak"); ax[2, 0].legend(); ax[2, 1].set_title("150-1k band"); ax[2, 2].set_title("2-15k band"); ax[2, 3].set_title("1/12-oct spectrum")
for a in ax[2, :3]: a.set_ylim(-50, 3); a.grid(alpha=0.3)
ax[2, 3].set_ylim(-60, 2); ax[2, 3].grid(alpha=0.3)
fig.tight_layout(); fig.savefig(f"plots/{NAME}_cmp.png", dpi=80)
