"""ALT-CELLO stage 2 iteration 2 chart: torsion, wound-string stiffness and calibrated radiation.

Run from the altcello directory after `make test` and charts/calibrate_radiation.py (needs out/sweep_body.txt,
out/sweep_body_c2.txt, out/t12_force.txt, the 12 wavs in audio/stage2b/, and the iteration 1 data in
/Users/alex/Music/foolish_cities_v5/alt_cello/charts/iter1_data/):
    nice /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/stage2b.py [OUT.png]
"""
import os, re, subprocess, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import calibrate_radiation as CR  # noqa: E402
M = CR.M

OUT = sys.argv[1] if len(sys.argv) > 1 else "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/stage2_body_iter2.png"
IT1 = "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/iter1_data/"
AUDIO = CR.AUDIO
EXE = os.path.join(ROOT, "altcello")
O = lambda n: os.path.join(ROOT, "out", n)
F0 = 146.83
BETA, VB = 0.1, 0.1
fs = 96000.0


def probe(prefix, extra, secs=4.2, F=0.638, string="D3"):
    r = subprocess.run(["nice", EXE, "probe", string, str(BETA), str(VB), str(F), str(secs), O(prefix)] + extra, capture_output=True, text=True, check=True)
    rtf = float(re.search(r"real-time factor ([0-9.]+)", r.stdout).group(1))
    d = np.fromfile(O(prefix) + ".f32", dtype="<f4").reshape(-1, 8).astype(float)
    return d, rtf


def fundamental(x, t0):
    seg = x[int(t0 * fs):]
    N = 1 << 21
    X = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), N))
    fr = np.fft.rfftfreq(N, 1 / fs)
    k = np.argmax(np.where((fr > F0 * .95) & (fr < F0 * 1.05), X, 0))
    a, b, c = np.log(X[k - 1:k + 2])
    return fr[k] + 0.5 * (a - c) / (a - 2 * b + c) * (fr[1] - fr[0])


def intervals(vrel):
    sl = np.abs(vrel) > 1e-4
    on = np.where(sl[1:] & ~sl[:-1])[0] + 1
    off = np.where(~sl[1:] & sl[:-1])[0] + 1
    off = off[off > on[0]]
    m = min(len(on), len(off))
    keep = (off[:m] - on[:m]) >= int(0.00015 * fs)
    return on[:m][keep], (off[:m] - on[:m])[keep]


def settle(vrel, P):
    on, _ = intervals(vrel)
    for a in range(len(on) - 10):
        dd = np.diff(on[a:a + 11])
        if np.all((dd > .85 * P) & (dd < 1.15 * P)):
            return on[a] / fs
    return float("nan")


def hump(x, f0):
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


def band(rows, beta):
    v = [r[1] for r in rows if abs(r[0] - beta) < 1e-6 and r[2] == "helmholtz"]
    return "%.2f-%.2f N" % (min(v), max(v)) if v else "none"


# ---------------------------------------------------------------- data
Fb = float(open(O("t12_force.txt")).read().split()[0])
bod, _ = probe("s2b_body", ["--body", "on"], F=Fb)
alls, rtf = probe("s2b_all", ["--body", "on", "--strings", "all"], F=Fb)
print("D3 body on F %.3f, whole instrument rtf %.2f" % (Fb, rtf))
t1 = np.loadtxt(IT1 + "stage2_iter1_force.txt")           # t_ms, force (4 periods, iteration 1, F 0.638 N)
s_it1 = sweep(IT1 + "stage2_iter1_sweep_body.txt")
s_it2 = sweep(O("sweep_body.txt"))
s_c2 = sweep(O("sweep_body_c2.txt"))
it1_hump, it1_settle, it1_slip, it1_pitch = -20.3, 0.593, 0.1263, -0.83   # T12 of iteration 1 (docs/TESTS), body on, D3 F 0.638 N

fb = fundamental(bod[:, 0], 3.0)
P = fs / fb
on, du = intervals(bod[int(3.0 * fs):, 3])
s0 = int(3.0 * fs) + on[0]
n4 = int(4 * P)
force2 = bod[s0:s0 + n4, 0]
slipf = du[:20].sum() / (np.diff(on[:21]).sum()) if len(on) > 21 else float("nan")
hump2 = hump(bod[:, 0], F0)
settle2 = settle(bod[:, 3], P)
pitch2 = 1200 * np.log2(fb / F0)

# calibration notes: measure the 12 final wavs
alt_rows, ys = CR.measure_alt({m: os.path.join(AUDIO, "stage2b_%d_%s.wav" % (m, s)) for s, m in CR.NOTES})
midis = [m for _, m in CR.NOTES if os.path.exists(CR.PHIL + "wav/ph_cello_%d.wav" % m)]
ph_rows, ph_ltas = CR.phil_rows(midis)
smp = CR.samples(alt_rows, ph_rows, midis)
mederr = CR.median_abs_err(smp)
ltas12 = CR.ltas_of([e for _, _, e in ys])
ltas11 = CR.ltas_of([e for m, _, e in ys if m in midis])
bd = CR.band_diff(ltas11, ph_ltas)
print("median abs harmonic error %.2f dB; LTAS band diffs %s" % (mederr, bd))

# ---------------------------------------------------------------- figure
C = {"blue": "#1f5fa8", "orange": "#d9730d", "green": "#2a8c55", "red": "#c0392b", "grey": "#555555", "purple": "#7b4fa3"}
plt.rcParams.update({"font.size": 12, "axes.grid": True, "grid.alpha": 0.25, "axes.spines.top": False, "axes.spines.right": False})
fig, ax = plt.subplots(2, 2, figsize=(16, 11), dpi=100)
fig.suptitle("ALT-CELLO - torsion, wound-string stiffness and calibrated radiation - stage 2 iteration 2", fontsize=17, fontweight="bold", y=0.985)

a = ax[0, 0]
a.plot(t1[:, 0], t1[:, 1], color=C["grey"], lw=1.4, ls="--", label="iteration 1 (solid-core stiffness, no torsion), F_B 0.638 N")
a.plot(np.arange(n4) / fs * 1e3, force2, color=C["blue"], lw=1.4, label="iteration 2 (wound EI, torsion), F_B %.3f N" % Fb)
a.set_xlabel("time (ms)")
a.set_ylabel("bridge force, T y_x - E I y_xxx (N)")
a.set_title("(a) bridge force over 4 periods, open D3, body on", loc="left", fontsize=14)
a.legend(loc="upper right", fontsize=10)

b = ax[0, 1]
col = {"helmholtz": C["green"], "multiple": C["orange"], "raucous": C["red"], "none": C["grey"]}
# rows (bottom to top): D3 beta 0.05, 0.1, 0.2 (iteration 1 open squares just above iteration 2 filled circles), then C2 beta 0.1, 0.15 (triangles)
rows_def = [("D3 beta 0.05", s_it1, s_it2, 0.05, 0), ("D3 beta 0.1", s_it1, s_it2, 0.1, 1), ("D3 beta 0.2", s_it1, s_it2, 0.2, 2),
            ("C2 beta 0.1", None, s_c2, 0.1, 3.4), ("C2 beta 0.15", None, s_c2, 0.15, 4.4)]
for lab, r1, r2, be, y in rows_def:
    for rows, dy, mk, filled in [(r1, 0.17, "s", False), (r2, -0.17 if r1 is not None else 0.0, "o" if r1 is not None else "^", True)]:
        if rows is None: continue
        for cls, cc in col.items():
            pts = [r[1] for r in rows if abs(r[0] - be) < 1e-6 and r[2] == cls]
            if pts:
                b.scatter(pts, [y + dy] * len(pts), s=46, marker=mk, facecolors=cc if filled else "none", edgecolors=cc, linewidths=1.6)
b.set_xscale("log")
b.set_xlim(0.03, 4)
b.set_ylim(-0.6, 7.0)
b.set_yticks([r[4] for r in rows_def]); b.set_yticklabels([r[0] for r in rows_def])
b.set_xlabel("bow force F_B (N)")
b.set_title("(b) Schelleng map, body on: D3 iteration 1 vs 2, and C2", loc="left", fontsize=14)
h = [Line2D([], [], marker="o", ls="", color=col[k], label=l) for k, l in [("helmholtz", "Helmholtz"), ("multiple", "multiple slipping"), ("raucous", "raucous"), ("none", "no slip")]]
h += [Line2D([], [], marker="s", ls="", mfc="none", color=C["grey"], label="D3 iteration 1 (open squares)"),
      Line2D([], [], marker="o", ls="", color=C["grey"], label="D3 iteration 2 (circles)"),
      Line2D([], [], marker="^", ls="", color=C["grey"], label="C2 iteration 2 (triangles)")]
b.legend(handles=h, loc="upper left", fontsize=9, ncol=3)

c_ = ax[1, 0]
want = [(36, "C2 open", C["blue"], "o", "-"), (50, "D3 open", C["orange"], "s", "--"), (64, "E4 stopped (A string)", C["green"], "^", ":")]
ks = np.arange(1, 17)
for midi, lab, cc, mk, ls in want:
    a_ = alt_rows[midi]
    alt = np.array([0.0] + [a_.get("h%d" % k, np.nan) for k in range(2, 17)])
    tm = midi if midi in ph_rows else 38           # C2 (MIDI 36) is below the Philharmonia range: nearest note D2
    p_ = ph_rows[tm]
    tgt = np.array([0.0] + [p_.get("h%d" % k, np.nan) for k in range(2, 17)])
    c_.plot(ks, tgt, color=cc, ls=ls, marker=mk, mfc="none", lw=1.3, ms=6, label="TARGET Philharmonia %s%s" % (lab.split(" ")[0], " (MIDI 38, no 36 sample)" if midi == 36 else ""))
    c_.plot(ks, alt, color=cc, ls="-", marker=mk, lw=2.0, ms=6, label="ALT-CELLO %s" % lab)
c_.set_xlabel("harmonic number")
c_.set_ylabel("level re h1 (dB)")
c_.set_xticks(ks)
c_.set_ylim(-60, 28)
c_.set_title("(c) harmonic levels: TARGET (open symbols, thin) vs ALT-CELLO (solid)", loc="left", fontsize=14)
c_.legend(loc="lower left", fontsize=8.5, ncol=2)

d_ = ax[1, 1]
d_.plot(M.LTAS_F, ltas12, color=C["blue"], lw=2.2, label="ALT-CELLO, 12 notes (MIDI 36 to 67)")
d_.plot(M.LTAS_F, ph_ltas, color=C["red"], lw=2.2, ls="--", label="TARGET Philharmonia, the same notes (%d: MIDI 38 to 67)" % len(midis))
d_.set_xscale("log"); d_.set_xlim(50, 16000); d_.set_ylim(-75, 4)
d_.set_xlabel("frequency (Hz)"); d_.set_ylabel("LTAS, 1/6 octave (dB re peak)")
d_.set_title("(d) long-term average spectrum of the 12 calibration notes", loc="left", fontsize=14)
d_.legend(loc="lower left", fontsize=10)

txt = ("D3 body on, F_B %.3f N, beta 0.1:  ripple hump 2.5-3.5 kHz %.1f dB re h1 (iteration 1: %.1f)   |   settling %.2f s (it. 1: %.2f s)   |   slip fraction %.3f   |   pitch %+.2f cents\n"
       "Helmholtz force band, body on, beta 0.05 / 0.1 / 0.2:  %s / %s / %s  (iteration 1: %s / %s / %s)   |   C2 beta 0.1: %s\n"
       "median |harmonic error| h2..h16, %d notes: %.2f dB   |   LTAS ALT-CELLO minus TARGET, 100-300 / 300-1000 / 1000-3000 / 3000-8000 Hz: %s dB\nreal-time factor %.1f x (4 strings + torsion + body, 96 kHz)"
       % (Fb, hump2, it1_hump, settle2, it1_settle, slipf, pitch2,
          band(s_it2, 0.05), band(s_it2, 0.1), band(s_it2, 0.2), band(s_it1, 0.05), band(s_it1, 0.1), band(s_it1, 0.2), band(s_c2, 0.1),
          len(midis), mederr, " / ".join("%+.1f" % x for x in bd), rtf))
fig.text(0.5, 0.008, txt, ha="center", va="bottom", fontsize=9.5, bbox=dict(boxstyle="round", fc="#f3f3f3", ec="#999999"))
fig.tight_layout(rect=(0, 0.095, 1, 0.955))
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT)
print("wrote", OUT)
print(txt)
