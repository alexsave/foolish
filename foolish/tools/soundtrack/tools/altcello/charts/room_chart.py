"""ALT-CELLO stage 3a chart: the room and microphone, and the match to the Philharmonia release.

    nice /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/room_chart.py [OUT.png]
needs out/room_match/ from charts/room_match.py (dry and best wavs, match.tsv, best.txt).
"""
import os, subprocess, sys, textwrap
import numpy as np
from scipy.io import wavfile
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import room_match as RM  # noqa: E402

OUT = sys.argv[1] if len(sys.argv) > 1 else "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/stage3a_room_iter1.png"
OD = RM.OUTD
FS = 48000
C = 343.2
TARGET_C, DRY_C, ROOM_C = "#222222", "#d9822b", "#1f7a8c"
TITLE = "ALT-CELLO - room and microphone: image-source early reflections plus FDN tail - matched to the Philharmonia release - stage 3a iteration 1"


def header(path):
    kv = {}
    for line in open(path):
        for tok in line.split():
            if "=" in tok: k, v = tok.split("=", 1); kv[k] = v
    return kv


def octave_rt60(ir, fc):
    N = 1 << int(np.ceil(np.log2(2 * len(ir))))
    X = np.fft.rfft(ir, N)
    f = np.fft.rfftfreq(N, 1 / FS)
    x = np.zeros_like(f); x[1:] = np.log2(f[1:] / fc) / 0.5
    g = 1 / np.sqrt(1 + x ** 12); g[0] = 0.0  # no DC, as in room.c
    b = np.fft.irfft(X * g, N)[:len(ir)]
    c = np.cumsum((b ** 2)[::-1])[::-1]
    c = 10 * np.log10(c / c[0] + 1e-300)
    i5, i25 = np.argmax(c <= -5), np.argmax(c <= -25)
    t = np.arange(len(c)) / FS
    sl = np.polyfit(t[i5:i25 + 1], c[i5:i25 + 1], 1)[0]
    return c, -60.0 / sl


def images(src, mic, dim, order):
    out = []
    for qx in (0, 1):
        for qy in (0, 1):
            for qz in (0, 1):
                for nx in range(-order, order + 1):
                    for ny in range(-order, order + 1):
                        for nz in range(-order, order + 1):
                            n, q = (nx, ny, nz), (qx, qy, qz)
                            o = sum(abs(n[a] - q[a]) + abs(n[a]) for a in range(3))
                            if o > order: continue
                            p = [(1 - 2 * q[a]) * src[a] + 2 * n[a] * dim[a] for a in range(3)]
                            out.append((o, np.linalg.norm(np.array(p) - mic), p))
    return out


def main():
    rt, dist, score = [float(x) for x in open(os.path.join(OD, "best.txt")).read().split()]
    # impulse through the CLI for the chosen room (the IR panel)
    imp = os.path.join(OD, "impulse.wav")
    x = np.zeros(4800, dtype=np.float32); x[100] = 1.0
    wavfile.write(imp, FS, x)
    wet = os.path.join(OD, "impulse_best.wav")
    subprocess.run(["nice", RM.EXE, "room", imp, wet, "--rt60", "%g" % rt, "--dist", "%g" % dist, "--pattern", "cardioid", "--no-noise"],
                   capture_output=True, text=True, check=True)
    ir = np.fromfile(wet + "_ir.f32", dtype="<f4").astype(float)
    h = header(wet + "_ir.txt")
    hl = open(wet + "_ir.txt").read().split("\n")
    geo = dict(tok.split("=") for tok in hl[1].split())
    dim = [float(v) for v in geo["room"].split(",")]
    src = np.array([float(v) for v in geo["src"].split(",")]); mic = np.array([float(v) for v in geo["mic"].split(",")])
    axis = np.array([float(v) for v in geo["mic_axis"].split(",")])
    rtm = [float(v) for v in hl[3].split("=")[1].split(",")]
    bands = [125, 250, 500, 1000, 2000, 4000, 8000]
    drr, direct, tx = float(h["drr_db"]), float(h["direct_s"]), float(h["crossover_s"])

    fig = plt.figure(figsize=(16, 11), dpi=100)
    fig.suptitle("\n".join(textwrap.wrap(TITLE, 105)), fontsize=17, fontweight="bold", y=0.985)
    gs = fig.add_gridspec(3, 2, left=0.06, right=0.975, top=0.885, bottom=0.15, hspace=0.45, wspace=0.17, height_ratios=[0.9, 0.7, 1.75])
    # (a) IR
    axa1 = fig.add_subplot(gs[0, 0]); axa2 = fig.add_subplot(gs[1, 0])
    t = np.arange(len(ir)) / FS
    m80 = t <= 0.08
    axa1.vlines(t[m80] * 1000, 0, ir[m80], color="#555555", lw=0.8)
    im = images(src, mic, dim, 2)
    first = {(1, ): None}
    cols = {"direct": "#d62728"}
    names = ["x=0 wall", "x=Lx wall", "y=0 wall", "y=Ly wall", "floor", "ceiling"]
    colors = ["#1f77b4", "#2ca02c", "#9467bd", "#8c564b", "#e377c2", "#17becf"]
    d0 = np.linalg.norm(src - mic) / C * 1000
    axa1.axvline(d0, color="#d62728", lw=1.6, alpha=0.8, label="direct %.1f ms" % d0)
    for k in range(6):
        a, w = k // 2, k % 2
        p = src.copy(); p[a] = -src[a] if w == 0 else 2 * dim[a] - src[a]
        tm = np.linalg.norm(p - mic) / C * 1000
        axa1.axvline(tm, color=colors[k], lw=1.4, alpha=0.8, label="%s %.1f ms" % (names[k], tm))
    o2 = sorted({round(r / C * 1000, 2) for o, r, p in im if o == 2})
    axa1.plot(o2, [axa1.get_ylim()[1] * 0.0] * len(o2), "|", color="#999999", ms=0)
    for tm in o2:
        if tm < 80: axa1.axvline(tm, color="#bbbbbb", lw=0.6, ls=":")
    axa1.axvline(tx * 1000, color="k", lw=1.0, ls="--"); axa1.text(tx * 1000 + 0.6, axa1.get_ylim()[0] * 0.95, "image / FDN crossover %.0f ms" % (tx * 1000), fontsize=8, va="bottom")
    axa1.set_ylim(axa1.get_ylim()[0], axa1.get_ylim()[1] * 1.02)
    axa1.set_xlim(0, 80); axa1.set_xlabel("time (ms)"); axa1.set_ylabel("IR amplitude (re 1 m)")
    axa1.set_title("(a) impulse response, first 80 ms: direct, 6 first-order images (coloured), order 2 (grey dotted)", fontsize=10.5, loc="left")
    axa1.legend(fontsize=7.5, ncol=2, loc="upper right", frameon=False, bbox_to_anchor=(1.0, 1.0))
    nbin = int(0.005 * FS)
    ne = len(ir) // nbin
    env = 10 * np.log10((ir[:ne * nbin].reshape(ne, nbin) ** 2).sum(1) + 1e-30)
    axa2.plot((np.arange(ne) + 0.5) * 5, env - env.max(), color=ROOM_C, lw=1.2)
    axa2.set_xlabel("time (ms)"); axa2.set_ylabel("5 ms energy (dB re peak)")
    axa2.set_title("(a) full-length energy envelope (decay to -60 dB: RT60 %.2f s mid band target)" % rt, fontsize=10.5, loc="left")
    axa2.set_ylim(-80, 3); axa2.set_xlim(0, min(len(ir) / FS, max(rtm) * 1.5) * 1000); axa2.grid(alpha=0.25)
    # (b) Schroeder
    axb = fig.add_subplot(gs[0:2, 1])
    cm = plt.get_cmap("viridis")
    for i, fc in enumerate(bands):
        c, r = octave_rt60(ir, fc)
        axb.plot(np.arange(len(c)) / FS, c, color=cm(i / 6.5), lw=1.5, label="%d Hz: RT60 %.2f s (T20 x 3)" % (fc, r))
    axb.axhspan(-25, -5, color="#dddddd", alpha=0.5, lw=0); axb.text(0.02, -15, "T20 fit\n-5 to -25 dB", fontsize=8, color="#555555", va="center")
    axb.set_ylim(-60, 2); axb.set_xlim(0, min(len(ir) / FS, 1.0 * max(rtm) * 1.1)); axb.set_xlabel("time (s)"); axb.set_ylabel("Schroeder backward integral (dB)")
    axb.set_title("(b) decay per octave band: low band x1.2, mid x1.0, high x0.7 of the %.1f s target" % rt, fontsize=10.5, loc="left")
    axb.legend(fontsize=9, loc="upper right", frameon=False); axb.grid(alpha=0.25)
    # (c) release
    axc = fig.add_subplot(gs[2, 0])
    tt = np.arange(0.0, 0.301, 0.01)
    ph = RM.phil_curves(tt)
    pc = np.array([c for _, _, c in ph])
    axc.fill_between(tt * 1000, np.percentile(pc, 10, 0), np.percentile(pc, 90, 0), color=TARGET_C, alpha=0.15, lw=0, label="TARGET Philharmonia p10-p90 (30 notes)")
    axc.plot(tt * 1000, np.median(pc, 0), color=TARGET_C, lw=2.4, label="TARGET Philharmonia median")
    dry, wetc = [], []
    for n, _, _ in RM.NOTES:
        yd, sr = RM.load(os.path.join(OD, "%s_dry.wav" % n)); dry.append(RM.curve(yd, sr, RM.LIFT, tt)[1])
        w = os.path.join(OD, "%s_best.wav" % n); yw, sr = RM.load(w)
        dl = float(header(w + "_ir.txt")["direct_s"]); wetc.append(RM.curve(yw, sr, RM.LIFT + dl, tt)[1])
    dry, wetc = np.array(dry), np.array(wetc)
    for a in dry: axc.plot(tt * 1000, a, color=DRY_C, lw=0.7, alpha=0.5)
    for a in wetc: axc.plot(tt * 1000, a, color=ROOM_C, lw=0.7, alpha=0.5)
    axc.plot(tt * 1000, np.median(dry, 0), color=DRY_C, lw=2.4, label="ALT-CELLO dry (median of G2 D3 A3)")
    axc.plot(tt * 1000, np.median(wetc, 0), color=ROOM_C, lw=2.4, label="ALT-CELLO + room %.1f s, %.1f m (median)" % (rt, dist))
    axc.set_ylim(-45, 4); axc.set_xlim(0, 300); axc.set_xlabel("time after the bow lifts (ms)"); axc.set_ylabel("level re sustain (dB)")
    axc.set_title("(c) release after the bow lifts (thin lines: the 3 ALT notes)", fontsize=10.5, loc="left")
    axc.legend(fontsize=8, loc="lower left", frameon=False); axc.grid(alpha=0.25)
    axc.axvspan(150, 300, color="#cc4444", alpha=0.06, lw=0)
    axc.text(225, 1.5, "Philharmonia files end 0.2-0.5 s\nafter the lift (edit fade)", fontsize=7.5, color="#993333", ha="center", va="center")
    # (d) plan
    axd = fig.add_subplot(gs[2, 1])
    axd.add_patch(plt.Rectangle((0, 0), dim[0], dim[1], fill=False, ec="k", lw=2))
    for k in range(6):
        a, w = k // 2, k % 2
        if a == 2: continue
        p = src.copy(); p[a] = -src[a] if w == 0 else 2 * dim[a] - src[a]
        axd.plot(p[0], p[1], "s", color=colors[k], ms=8); axd.text(p[0], p[1] + 0.2, names[k], fontsize=7.5, ha="center", color=colors[k])
        axd.plot([src[0], p[0]], [src[1], p[1]], ":", color=colors[k], lw=0.8)
        axd.plot([p[0], mic[0]], [p[1], mic[1]], "-", color=colors[k], lw=0.8, alpha=0.6)
    axd.plot(src[0], src[1], "o", color="#d62728", ms=11)
    axd.annotate("", xy=(src[0], src[1] - 0.8), xytext=(src[0], src[1]), arrowprops=dict(arrowstyle="->", color="#d62728", lw=2))
    axd.text(src[0] + 0.25, src[1] + 0.1, "cello, front faces down\n(f-hole region, z %.2f m)" % src[2], color="#d62728", fontsize=8, va="bottom")
    axd.plot(mic[0], mic[1], "^", color="k", ms=11)
    axd.annotate("", xy=(mic[0] + 0.8 * axis[0], mic[1] + 0.8 * axis[1]), xytext=(mic[0], mic[1]), arrowprops=dict(arrowstyle="->", color="k", lw=1.6))
    axd.text(mic[0] + 0.3, mic[1] - 0.6, "mic %.1f m in front, z %.1f m\ncardioid, aimed at the cello" % (dist, mic[2]), color="k", fontsize=8, va="top")
    axd.set_xlim(-4.2, dim[0] + 4.2); axd.set_ylim(-4.6, dim[1] + 2.0); axd.set_aspect("equal")
    axd.set_xlabel("X (m)"); axd.set_ylabel("Y (m)")
    axd.set_title("(d) plan view, %.0f x %.0f m room (height %.1f): cello, mic, wall images" % tuple(dim), fontsize=10.5, loc="left")
    # key numbers
    mt = {}
    for line in open(os.path.join(OD, "match.tsv")):
        if line.startswith("#"): continue
        p = line.split(); mt[(p[0], p[1], p[2])] = [float(v) for v in p[3:]]
    txt = ("Key numbers.  Room: Eyring RT60 target %.2f s mid (low x1.2, high x0.7); measured per octave 125 Hz-8 kHz: %s s.  "
           "DRR %.1f dB at %.1f m (cardioid, aimed at the source).  Image sources to order 6, complete to %.0f ms, FDN 8 lines after.  "
           "Mic self-noise 12 dB(A) re 74 dB SPL nominal.\n"
           "Release re sustain at 50/100/150/200 ms: TARGET Philharmonia %s dB; ALT-CELLO dry %s dB; ALT-CELLO + room %s dB (rms difference to TARGET %.1f dB; dry %.1f dB).  "
           "The 12 room candidates differ by only 1.5 dB, so the room is NOT identified by this match.") % (
        rt, " ".join("%.2f" % r for r in rtm), drr, dist, tx * 1000,
        "/".join("%.0f" % v for v in mt[("philharmonia", "0", "0")][1:5]), "/".join("%.0f" % v for v in mt[("dry", "0", "0")][1:5]),
        "/".join("%.0f" % v for v in mt[("room", "%.1f" % rt, "%.1f" % dist)][1:5]), score, mt[("dry", "0", "0")][5])
    fig.text(0.06, 0.012, "\n".join(textwrap.wrap(txt, 170)), fontsize=9.5, va="bottom")
    fig.savefig(OUT)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
