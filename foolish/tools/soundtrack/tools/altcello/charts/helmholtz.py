"""ALT-CELLO stage 1 chart: Helmholtz motion of the bowed D3 string.

Run from the altcello directory after `make test` (needs out/sweep.txt and
out/t3_force.txt written by the tests):
    /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/helmholtz.py [OUT.png]
It calls `altcello probe` for the T3 condition and reads the probe output.
"""
import os, re, subprocess, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = sys.argv[1] if len(sys.argv) > 1 else "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/stage1_helmholtz_iter1.png"
F0 = 146.83
BETA, VB = 0.1, 0.1

def read_kv(path):
    d = {}
    for t in open(path).read().split():
        if "=" in t:
            k, v = t.split("=", 1)
            d[k] = v
    return d

F = float(open(os.path.join(ROOT, "out/t3_force.txt")).read().split()[0])
prefix = os.path.join(ROOT, "out/t3")
res = subprocess.run(["nice", os.path.join(ROOT, "altcello"), "probe", "D3", str(BETA), str(VB), str(F), "4.2", prefix],
                     capture_output=True, text=True, check=True)
rtf = float(re.search(r"real-time factor ([0-9.]+)", res.stdout).group(1))
hdr = read_kv(prefix + ".txt")
fs = float(hdr["fs"])
nch = int(hdr["channels"])
d = np.fromfile(prefix + ".f32", dtype="<f4").reshape(-1, nch)
bridge_u, vbow, vrel = d[:, 0].astype(float), d[:, 2].astype(float), d[:, 3].astype(float)

# fundamental from the settled part (3.0 s on), Hann + zero padded fft, parabolic peak
i0 = int(3.0 * fs)
seg = bridge_u[i0:]
N = 1 << 21
X = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), N))
fr = np.fft.rfftfreq(N, 1 / fs)
m = (fr > F0 * 0.95) & (fr < F0 * 1.05)
k = np.argmax(np.where(m, X, 0))
a, b, c = np.log(X[k - 1:k + 2])
f_est = fr[k] + 0.5 * (a - c) / (a - 2 * b + c) * (fr[1] - fr[0])
cents = 1200 * np.log2(f_est / F0)
P = fs / f_est

# slip fraction over 20 periods
sl = np.abs(vrel) > 1e-4
start = i0
while not (not sl[start] and sl[start + 1]):
    start += 1
w1 = start + int(20 * P)
runs, i, big = 0, start, 0
while i < w1:
    if sl[i]:
        j = i
        while sl[j]:
            j += 1
        if j - i >= int(0.00015 * fs):
            big += j - i
        i = j
    else:
        i += 1
slip_frac = big / (w1 - start)

# sweep
sw = []
for line in open(os.path.join(ROOT, "out/sweep.txt")):
    if line.startswith("#"):
        continue
    p = line.split()
    sw.append((float(p[0]), float(p[1]), p[2]))
sw = np.array(sw, dtype=object)
def band(beta):
    fs_ = [r[1] for r in sw if abs(r[0] - beta) < 1e-6 and r[2] == "helmholtz"]
    return (min(fs_), max(fs_)) if fs_ else (None, None)
Fmin, Fmax = band(0.1)

# window of 4 periods starting at a slip onset
w0 = start
n4 = int(4 * P)
t = (np.arange(n4)) / fs * 1e3

C = {"blue": "#1f5fa8", "orange": "#d9730d", "green": "#2a8c55", "red": "#c0392b", "grey": "#555555", "purple": "#7b4fa3"}
plt.rcParams.update({"font.size": 12, "axes.grid": True, "grid.alpha": 0.25, "axes.spines.top": False, "axes.spines.right": False})
fig, ax = plt.subplots(2, 2, figsize=(16, 11), dpi=100)
fig.suptitle("ALT-CELLO - bowed D3 string - Helmholtz motion - stage 1 iteration 1", fontsize=22, fontweight="bold", y=0.985)

# (a)
a = ax[0, 0]
a.plot(t, vbow[w0:w0 + n4], color=C["blue"], lw=1.6, label="ALT-CELLO string velocity at the bow")
a.plot(t, np.full(n4, VB), color=C["orange"], ls="--", lw=2, label="bow velocity (%.2f m/s)" % VB)
a.set_title("(a) string velocity at the bow, 4 periods", loc="left", fontsize=14)
a.set_xlabel("time (ms)")
a.set_ylabel("velocity (m/s)")
a.legend(loc="lower right")

# (b) ideal Helmholtz sawtooth (Raman): linear ramp over the sticking part, fast jump, aligned by correlation
u = bridge_u[w0:w0 + n4]
pp = u.max() - u.min()
ph = np.arange(n4) / P
best, bests = -2, 0
for sh in np.linspace(0, 1, 200, endpoint=False):
    saw = 0.5 - ((ph + sh) % 1.0)
    cc = np.corrcoef(saw, u)[0, 1]
    if cc > best:
        best, bests = cc, sh
saw = (0.5 - ((ph + bests) % 1.0)) * pp + 0.5 * (u.max() + u.min())
b_ = ax[0, 1]
b_.plot(t, u, color=C["blue"], lw=1.6, label="ALT-CELLO bridge force")
b_.plot(t, saw, color=C["red"], ls="--", lw=2, label="TARGET ideal Helmholtz sawtooth (Raman)")
b_.set_title("(b) bridge force (u polarisation), 4 periods", loc="left", fontsize=14)
b_.set_xlabel("time (ms)")
b_.set_ylabel("force (N)")
b_.legend(loc="upper right")

# (c) Schelleng map
c_ = ax[1, 0]
style = {"helmholtz": (C["green"], "o", "Helmholtz"), "multiple": (C["orange"], "s", "multiple slipping"), "raucous": (C["red"], "x", "raucous"), "none": (C["grey"], "+", "no stick")}
for cls, (col, mk, lab) in style.items():
    pts = [(r[1], r[0]) for r in sw if r[2] == cls]
    if pts:
        pts = np.array(pts)
        c_.scatter(pts[:, 0], pts[:, 1], c=col, marker=mk, s=70, label=lab, linewidths=2)
if Fmin:
    fgrid = np.logspace(np.log10(0.02), np.log10(10), 100)
    c_.plot(fgrid, BETA * Fmax / fgrid, color=C["purple"], lw=2, label="Schelleng F_max ~ 1/beta (anchored at beta=0.1)")
    c_.plot(fgrid, BETA * np.sqrt(Fmin / fgrid), color=C["grey"], lw=2, ls="--", label="Schelleng F_min ~ 1/beta^2 (anchored at beta=0.1)")
c_.set_xscale("log")
c_.set_yscale("log")
c_.set_ylim(0.035, 0.3)
c_.set_xlim(0.015, 12)
c_.set_yticks([0.05, 0.1, 0.2])
c_.set_yticklabels(["0.05", "0.1", "0.2"])
c_.set_xlabel("bow force F_B (N)")
c_.set_ylabel("relative bow position beta")
c_.set_title("(c) Schelleng sweep, v_B = 0.1 m/s (8 s runs, last 0.5 s classified)", loc="left", fontsize=14)
c_.legend(loc="lower left", fontsize=10)

# (d) spectrum
d_ = ax[1, 1]
seg = bridge_u[i0:i0 + int(1.0 * fs)]
S = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), 1 << 18))
fq = np.fft.rfftfreq(1 << 18, 1 / fs)
db = 20 * np.log10(S / S.max() + 1e-12)
mk = fq <= 5000
d_.plot(fq[mk], db[mk], color=C["blue"], lw=1.0, label="ALT-CELLO bridge force spectrum")
for h in range(1, int(5000 / f_est) + 1):
    d_.axvline(h * f_est, color=C["red"], alpha=0.25, lw=1)
    if h <= 20:
        d_.text(h * f_est, 3, str(h), ha="center", va="bottom", fontsize=9, color=C["red"])
d_.plot([], [], color=C["red"], alpha=0.5, label="harmonic numbers n x %.2f Hz" % f_est)
d_.set_ylim(-100, 12)
d_.set_xlim(0, 5000)
d_.set_xlabel("frequency (Hz)")
d_.set_ylabel("dB re max")
d_.set_title("(d) bridge force spectrum", loc="left", fontsize=14)
d_.legend(loc="upper right")

txt = ("slip fraction %.3f (beta %.2f)   |   pitch %+.2f cents vs open D3 (%.2f Hz)   |   F_min %s N, F_max %s N at beta 0.1   |   real-time factor %.1f x (96 kHz, D3)\n"
       "probe: beta %.2f, v_B %.2f m/s, F_B %.3f N, correlation with the ideal sawtooth %.3f"
       % (slip_frac, BETA, cents, F0, "%.3f" % Fmin if Fmin else "n/a", "%.3f" % Fmax if Fmax else "n/a", rtf, BETA, VB, F, best))
fig.text(0.5, 0.012, txt, ha="center", va="bottom", fontsize=12, bbox=dict(boxstyle="round", fc="#f3f3f3", ec="#999999"))
fig.tight_layout(rect=(0, 0.06, 1, 0.955))
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT)
print("wrote", OUT, "slip_frac %.4f cents %.3f Fmin %s Fmax %s rtf %.1f saw_corr %.3f" % (slip_frac, cents, Fmin, Fmax, rtf, best))
