"""ALT-CELLO stage 2 chart: body admittance, coupled Helmholtz motion, radiated spectrum.

Run from the altcello directory after `make test` (needs out/sweep.txt,
out/sweep_body.txt, out/t12_force.txt):
    /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/stage2_body.py [OUT.png]
"""
import os, re, subprocess, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = sys.argv[1] if len(sys.argv) > 1 else "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/stage2_body_iter1.png"
F0 = 146.83
BETA, VB = 0.1, 0.1
EXE = os.path.join(ROOT, "altcello")
O = lambda n: os.path.join(ROOT, "out", n)

def probe(prefix, extra, secs=4.2, F=0.638):
    r = subprocess.run(["nice", EXE, "probe", "D3", str(BETA), str(VB), str(F), str(secs), O(prefix)] + extra, capture_output=True, text=True, check=True)
    rtf = float(re.search(r"real-time factor ([0-9.]+)", r.stdout).group(1))
    d = np.fromfile(O(prefix) + ".f32", dtype="<f4").reshape(-1, 8).astype(float)
    return d, rtf

def fundamental(x, fs, t0):
    seg = x[int(t0 * fs):]
    N = 1 << 21
    X = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), N))
    fr = np.fft.rfftfreq(N, 1 / fs)
    k = np.argmax(np.where((fr > F0 * .95) & (fr < F0 * 1.05), X, 0))
    a, b, c = np.log(X[k - 1:k + 2])
    return fr[k] + 0.5 * (a - c) / (a - 2 * b + c) * (fr[1] - fr[0])

def intervals(vrel, fs):
    sl = np.abs(vrel) > 1e-4
    on = np.where(sl[1:] & ~sl[:-1])[0] + 1
    off = np.where(~sl[1:] & sl[:-1])[0] + 1
    off = off[off > on[0]]
    m = min(len(on), len(off))
    keep = (off[:m] - on[:m]) >= int(0.00015 * fs)
    return on[:m][keep], (off[:m] - on[:m])[keep]

def settle(vrel, fs, P):
    on, _ = intervals(vrel, fs)
    for a in range(len(on) - 10):
        dd = np.diff(on[a:a + 11])
        if np.all((dd > .85 * P) & (dd < 1.15 * P)):
            return on[a] / fs
    return float("nan")

def hump(x, fs, f0):
    seg = x[-int(fs):]
    X = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), 1 << 19))
    fr = np.fft.rfftfreq(1 << 19, 1 / fs)
    h1 = X[(fr > f0 * .95) & (fr < f0 * 1.05)].max()
    return 20 * np.log10(X[(fr > 2500) & (fr < 3500)].max() / h1)

def sweep(path):
    r = []
    for line in open(path):
        if line.startswith("#"): continue
        p = line.split(); r.append((float(p[0]), float(p[1]), p[2]))
    return r

# data
subprocess.run(["nice", EXE, "admittance", O("body")], check=True)
adm = np.loadtxt(O("body_adm.txt"))
modes = [l.split() for l in open(O("body_modes.txt"))]
Fb = float(open(O("t12_force.txt")).read().split()[0])
fs = 96000.0
rig, _ = probe("s2_rig", [], F=0.638)
bod, _ = probe("s2_body", ["--body", "on"], F=Fb)
alls, rtf = probe("s2_all", ["--body", "on", "--strings", "all"], secs=4.2, F=Fb)
long, _ = probe("s2_long", ["--body", "on", "--strings", "all"], secs=5.0, F=Fb)

C = {"blue": "#1f5fa8", "orange": "#d9730d", "green": "#2a8c55", "red": "#c0392b", "grey": "#555555", "purple": "#7b4fa3"}
plt.rcParams.update({"font.size": 12, "axes.grid": True, "grid.alpha": 0.25, "axes.spines.top": False, "axes.spines.right": False})
fig, ax = plt.subplots(2, 2, figsize=(16, 11), dpi=100)
fig.suptitle("ALT-CELLO - body: bridge admittance, coupled Helmholtz motion and the radiated spectrum - stage 2 iteration 1", fontsize=15.5, fontweight="bold", y=0.985)

# (a) admittance
a = ax[0, 0]
a.plot(adm[:, 0], adm[:, 1], color=C["blue"], lw=1.4, label="|Y_uu| (bowing direction)")
a.plot(adm[:, 0], adm[:, 2], color=C["orange"], lw=1.4, label="|Y_ww| (normal to the top)")
a.set_xscale("log")
a.set_xlim(50, 10000)
a.set_ylim(-75, -5)
for nm in ["A0", "CBR", "B1-", "B1+"]:
    m = [x for x in modes if x[0] == nm][0]
    f = float(m[1])
    a.axvline(f, color=C["grey"], alpha=.4, lw=1)
    a.text(f, -7.5, nm, rotation=90, va="top", ha="right", fontsize=10, color=C["grey"])
a.axvspan(1500, 2700, color=C["purple"], alpha=.12, label="bridge hill (rocking resonance, 2 kHz, Q 3)")
a.set_xlabel("frequency (Hz)")
a.set_ylabel("admittance (dB re 1 s/kg)")
a.set_title("(a) bridge admittance of the modal body", loc="left", fontsize=14)
a.legend(loc="lower right", fontsize=10)

# (b) sawtooth rigid vs body
b = ax[0, 1]
for d, col, lab, ls in [(rig, C["grey"], "rigid bridge (stage 1)", "-"), (bod, C["blue"], "body on (stage 2)", "-")]:
    f = fundamental(d[:, 0], fs, 3.0)
    P = fs / f
    on, _ = intervals(d[int(3.0 * fs):, 3], fs)
    s0 = int(3.0 * fs) + on[0]
    n4 = int(4 * P)
    b.plot(np.arange(n4) / fs * 1e3, d[s0:s0 + n4, 0], color=col, lw=1.4, label=lab, ls=ls)
b.set_xlabel("time (ms)")
b.set_ylabel("bridge force, T y_x - E I y_xxx (N)")
b.set_title("(b) bridge force over 4 periods, F_B %.3f N" % Fb, loc="left", fontsize=14)
b.legend(loc="upper right")

# (c) Schelleng maps
c_ = ax[1, 0]
s1 = sweep(O("sweep.txt")); s2 = sweep(O("sweep_body.txt"))
col = {"helmholtz": C["green"], "multiple": C["orange"], "raucous": C["red"], "none": C["grey"]}
for rows, off, filled in [(s1, 0.90, False), (s2, 1.10, True)]:
    for cls, cc in col.items():
        pts = [(r[1], r[0] * off) for r in rows if r[2] == cls]
        if pts:
            p = np.array(pts)
            c_.scatter(p[:, 0], p[:, 1], s=45, marker="o" if filled else "s", facecolors=cc if filled else "none", edgecolors=cc, linewidths=1.6)
c_.set_xscale("log"); c_.set_yscale("log")
c_.set_ylim(0.035, 0.7); c_.set_xlim(0.015, 12)
c_.set_yticks([0.05, 0.1, 0.2]); c_.set_yticklabels(["0.05", "0.1", "0.2"])
c_.set_xlabel("bow force F_B (N)")
c_.set_ylabel("relative bow position beta")
c_.set_title("(c) Schelleng map: stage 1 rigid (open squares, below) vs body on (filled circles, above)", loc="left", fontsize=12)
from matplotlib.lines import Line2D
h = [Line2D([], [], marker="o", ls="", color=col[k], label=l) for k, l in [("helmholtz", "Helmholtz"), ("multiple", "multiple slipping"), ("raucous", "raucous")]]
h += [Line2D([], [], marker="s", ls="", mfc="none", color=C["grey"], label="stage 1, rigid bridge"), Line2D([], [], marker="o", ls="", color=C["grey"], label="stage 2, body on")]
c_.legend(handles=h, loc="upper right", fontsize=9, ncol=2)

# (d) body pressure spectrum
d_ = ax[1, 1]
p = long[int(2.0 * fs):, 7]
f = fundamental(long[:, 0], fs, 2.0)
S = np.abs(np.fft.rfft(p * np.hanning(len(p)), 1 << 19)); fr = np.fft.rfftfreq(1 << 19, 1 / fs)
db = 20 * np.log10(S / S.max() + 1e-12)
d_.plot(fr[(fr > 50) & (fr < 20000)], db[(fr > 50) & (fr < 20000)], color=C["blue"], lw=0.8, label="ALT-CELLO body pressure, bowed open D3, all strings")
for hh in range(1, 41):
    d_.axvline(hh * f, color=C["red"], alpha=.15, lw=.8)
for hh in [1, 2, 3, 5, 10, 20, 40]:
    d_.text(hh * f, 2, str(hh), ha="center", va="bottom", fontsize=8, color=C["red"])
d_.plot([], [], color=C["red"], alpha=.5, label="harmonic numbers n x %.2f Hz" % f)
d_.set_xscale("log"); d_.set_xlim(50, 20000); d_.set_ylim(-110, 12)
d_.set_xlabel("frequency (Hz)"); d_.set_ylabel("dB re max")
d_.set_title("(d) body-pressure spectrum, 2 s of bowed open D3", loc="left", fontsize=14)
d_.legend(loc="lower left", fontsize=10)

# metrics
fb = fundamental(bod[:, 0], fs, 3.0)
on, du = intervals(bod[int(3.0 * fs):, 3], fs)
slipf = du[:20].sum() / (np.diff(on[:21]).sum()) if len(on) > 21 else float("nan")
def band(rows, beta):
    v = [r[1] for r in rows if abs(r[0] - beta) < 1e-6 and r[2] == "helmholtz"]
    return "%.2f-%.2f N" % (min(v), max(v)) if v else "none"
txt = ("body on, F_B %.3f N, beta 0.1:  hump 2.5-3.5 kHz %.1f dB re h1 (rigid %.1f)   |   settling %.2f s (rigid %.2f s)   |   slip fraction %.3f   |   pitch %+.2f cents\n"
       "Helmholtz force band beta 0.05 / 0.1 / 0.2:  %s / %s / %s  (rigid %s / %s / %s)   |   real-time factor %.1f x (4 strings + body, 96 kHz)"
       % (Fb, hump(bod[:, 0], fs, F0), hump(rig[:, 0], fs, F0), settle(bod[:, 3], fs, fs / fb), settle(rig[:, 3], fs, fs / fundamental(rig[:, 0], fs, 3.0)), slipf, 1200 * np.log2(fb / F0),
          band(s2, 0.05), band(s2, 0.1), band(s2, 0.2), band(s1, 0.05), band(s1, 0.1), band(s1, 0.2), rtf))
fig.text(0.5, 0.012, txt, ha="center", va="bottom", fontsize=10.5, bbox=dict(boxstyle="round", fc="#f3f3f3", ec="#999999"))
fig.tight_layout(rect=(0, 0.07, 1, 0.955))
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT)
print("wrote", OUT)
print(txt)
