"""Chart of the second onset iteration: Guettler-wedge attack and smooth expert release (iteration 2). Needs out/wedge/map_G2onC.txt (charts/wedge_run.sh), out/wedge/final_{phil,c3,c4}.txt
(charts/wedge_final.py), the traces out/wedge/rel_c3expert, rel_c3novice, rel_expert2, rel_novice2 (charts/release_exp.sh, the old renderer for c3) and the candidate files.
    python -I charts/onset2.py CAND4_DIR -> /Users/alex/Music/foolish_cities_v5/alt_cello/charts/onset_iter2.png"""
import os, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import onset_lib as L  # noqa: E402
import release_study as RS  # noqa: E402
M = L.M
OUTPNG = "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/onset_iter2.png"
C3D = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/candidate3"
S3, S4 = {43: 0, 50: 2, 57: 0}, {43: 1, 50: 1, 57: 2}  # the seeds of the candidate notes (best of three by time to periodic)
CP, C3C, C4C, CN = "k", "#3b6ea5", "#e07a1f", "#7b3f9e"
W = os.path.join(L.ROOT, "out", "wedge")


def load_map(p):
    return np.array([[float(x) for x in l.split()] for l in open(p) if not l.startswith("#")])


def env_at(y, t_w):
    """20 ms rms level in dB re the sustain (median of the 10 ms hop frames from 0.5 to 1.4 s after t_w), time axis = frame start minus t_w (the measure of
    charts/attack_scan2_lib.py lev, so the curve passes through the numbers of the gates at +20/+50/+100 ms)"""
    lin = M.rms_env(y)
    env = 20 * np.log10(lin + 1e-12)
    t = np.arange(len(env)) * 0.01 - t_w
    sus = float(np.median(env[(t >= 0.5) & (t <= 1.4)]))
    return t, env - sus


def phil_start(midi):
    y = M.load_audio(os.path.join(L.G.PHIL_FULL % midi)); return y, L.G.onset_s(y)


def rows(label):
    d = {}
    for l in open(os.path.join(W, "final_%s.txt" % label)):
        q = l.split()
        if q and q[0] == "ROW": d.setdefault(q[2], []).append([float(q[5])] + [float(x) for x in q[7:10]] + [float(q[11]), float(q[13])])
    return {k: np.array(v) for k, v in d.items()}


def ctrl(prefix):
    return np.fromfile(prefix + "_ctrl.f32", dtype=np.float32).reshape(-1, 11)


def main():
    c4d = sys.argv[1]
    R = load_map(os.path.join(W, "map_G2onC.txt"))
    Fs = sorted(set(R[:, 0])); As = sorted(set(R[:, 1]))
    Z = np.full((len(Fs), len(As)), np.nan); Lv = np.full_like(Z, np.nan)
    for i, F in enumerate(Fs):
        for j, a in enumerate(As):
            r = R[(R[:, 0] == F) & (R[:, 1] == a)][0]
            Z[i, j] = r[2] if r[2] >= 0 else 1.0; Lv[i, j] = r[8]
    X, Y = np.meshgrid(np.log2(As), np.log10(Fs))
    fig = plt.figure(figsize=(16, 11), dpi=100)
    fig.suptitle("ALT-CELLO - confident starts: Guettler-wedge attack and a smooth expert release - iteration 2", fontsize=17, fontweight="bold", y=0.985)
    gs = fig.add_gridspec(3, 6, left=0.055, right=0.985, top=0.915, bottom=0.12, hspace=0.5, wspace=1.35)
    ax = fig.add_subplot(gs[0, 0:2])
    im = ax.pcolormesh(X, Y, np.minimum(Z, 0.5), cmap="viridis_r", shading="nearest", vmin=0, vmax=0.5)
    cs = ax.contour(X, Y, Lv, levels=[-10.8, -6.8, -2.8], colors=["w", "r", "w"], linewidths=[1.2, 1.8, 1.2], linestyles=["--", "-", "--"])
    ax.clabel(cs, fmt=lambda v: "%.1f" % v, fontsize=8)
    ax.set_xticks(np.log2(As)); ax.set_xticklabels(["%g" % a for a in As]); ax.set_yticks(np.log10(Fs[::2])); ax.set_yticklabels(["%.2f" % f for f in Fs[::2]])
    ax.set_xlabel("bow acceleration (m/s^2)"); ax.set_ylabel("bow force (N)")
    ax.plot(np.log2(1.5), np.log10(1.0), "w*", ms=14, mec="k", label="chosen: 1.5 m/s^2, 1.0 N")
    ax.legend(loc="lower left", fontsize=7.5)
    plt.colorbar(im, ax=ax, label="time to periodic, s (0.5 = slower)")
    ax.set_title("G2 on C: Guettler map; contours = level re sustain\nat 50 ms (red: Philharmonia -6.8, white: +-4 dB)", fontsize=9.5)
    # level of the first 0.3 s
    for k, (nm, t, midi) in enumerate((("G2", 0.4, 43), ("D3", 3.4, 50), ("A3", 6.4, 57))):
        ax = fig.add_subplot(gs[0, 2 + 2 * (k % 2):4 + 2 * (k % 2)] if k < 2 else gs[1, 0:2])
        yp, tp0 = phil_start(midi)
        y3 = M.load_audio(os.path.join(L.TMP, "cand3", "notes_s%d_dry.wav" % S3[midi])); y4 = M.load_audio(os.path.join(L.TMP, "cand4", "notes_s%d_dry.wav" % S4[midi]))
        tw = {43: 0.5, 50: 3.5, 57: 6.5}[midi]
        for y, lab, col, lw, t0 in ((yp, "TARGET Philharmonia", CP, 2.6, tp0), (y3, "candidate 3 (seed %d)" % S3[midi], C3C, 1.8, tw), (y4, "candidate 4 (seed %d)" % S4[midi], C4C, 2.2, tw)):
            tt, lv = env_at(y, t0)
            m = (tt >= -0.05) & (tt <= 0.3)
            ax.plot(tt[m], lv[m], color=col, lw=lw, label=lab)
        ax.axvline(0.05, color="0.7", lw=0.8, ls=":")
        ax.set_ylim(-35, 6); ax.set_xlabel("s after the written start (Philharmonia: its 2 percent onset)"); ax.set_ylabel("level re sustain, dB"); ax.set_title("%s: first 0.3 s level" % nm, fontsize=10); ax.grid(alpha=0.25)
        if k == 0: ax.legend(fontsize=8, loc="lower right")
    # 3. attack contours c3 vs c4 (G2, note 1: start 0.5 s)
    c3c, c4c = ctrl(os.path.join(W, "rel_c3expert")), ctrl(os.path.join(W, "rel_expert2"))
    ax = fig.add_subplot(gs[1, 2:4])
    t = np.arange(len(c4c)) / 1000.0
    for c, lab, col in ((c3c, "candidate 3", C3C), (c4c, "candidate 4", C4C)):
        ax.plot(t - 0.5, c[:, 1], color=col, lw=2, label=lab + " force (N)")
        ax.plot(t - 0.5, c[:, 0] * 10, color=col, lw=1.4, ls="--", label=lab + " speed x10 (m/s)")
    ax.set_xlim(-0.04, 0.45); ax.set_xlabel("s after the written start"); ax.set_title("G2 attack: bow force and speed", fontsize=10); ax.legend(fontsize=7, ncol=2); ax.grid(alpha=0.25)
    ax = fig.add_subplot(gs[1, 4:6])
    for c, lab, col in ((c3c, "candidate 3", C3C), (c4c, "candidate 4", C4C)):
        ax.plot(t - 2.0, c[:, 1], color=col, lw=2, label=lab + " force (N)")
        ax.plot(t - 2.0, c[:, 0] * 10, color=col, lw=1.4, ls="--", label=lab + " speed x10")
        ax.plot(t - 2.0, c[:, 5], color=col, lw=1.0, ls=":", label=lab + " finger press")
    ax.set_xlim(-0.15, 0.45); ax.set_xlabel("s after the written end"); ax.set_title("G2 release: force, speed, finger press (c4: finger held 0.3 s)", fontsize=10); ax.legend(fontsize=6.5, ncol=2); ax.grid(alpha=0.25)
    # 4. release window novice vs expert (levels)
    ax = fig.add_subplot(gs[2, 0:3])
    for pre, lab, col, ls in (("rel_novice2", "novice (c3 = c4)", CN, "-"), ("rel_c3expert", "expert, candidate 3", C3C, "-"), ("rel_expert2", "expert, candidate 4", C4C, "-")):
        r = RS.windows(os.path.join(W, pre + ".wav"), os.path.join(W, pre), [2.0, 5.0, 8.0], [43, 50, 57])
        ax.plot(r[0]["t"], r[0]["lev"], color=col, lw=2.2, ls=ls, label="%s: G2 bump %.1f dB (D3 %.1f, A3 %.1f)" % (lab, r[0]["bump_db"], r[1]["bump_db"], r[2]["bump_db"]))
    ax.axvline(0, color="0.5", lw=0.8, ls=":")
    ax.set_xlabel("s after the written end of G2"); ax.set_ylabel("level re sustain, dB"); ax.set_ylim(-45, 8); ax.grid(alpha=0.25); ax.legend(fontsize=8, loc="lower left")
    ax.set_title("release window, G2 (bump = largest rise after the lift)", fontsize=10)
    # 5. gate bars
    ax = fig.add_subplot(gs[2, 3:6])
    P, A3, A4 = rows("phil"), rows("c3"), rows("c4")
    groups = [("G2 lev50", "G2", 2), ("D3 lev50", "D3", 2), ("A3 lev50", "A3", 2), ("G2 t_per x100", "G2", 0), ("D3 t_per x100", "D3", 0), ("A3 t_per x100", "A3", 0), ("G2 att tilt", "G2", 4), ("D3 att tilt", "D3", 4)]
    xs = np.arange(len(groups))
    for off, R_, lab, col in ((-0.27, P, "Philharmonia", CP), (0.0, A3, "candidate 3", C3C), (0.27, A4, "candidate 4", C4C)):
        v = []
        for _, n, k in groups:
            x = float(np.median(R_[n][:, k]))
            v.append(x * 100 if k == 0 else x)
        ax.bar(xs + off, v, 0.25, color=col, label=lab)
        for xx, vv in zip(xs + off, v): ax.text(xx, vv + (0.6 if vv >= 0 else -2.2), "%.0f" % vv, ha="center", fontsize=6.5)
    ax.set_xticks(xs); ax.set_xticklabels([g[0] for g in groups], fontsize=7.5, rotation=20); ax.axhline(0, color="k", lw=0.6); ax.legend(fontsize=8); ax.grid(alpha=0.25, axis="y")
    ax.set_title("gate bars (median of 3 seeds): lev50 = level re sustain at 50 ms (dB),\nt_per = audio time to periodic (x100 s), att tilt = 2-6 kHz attack minus sustain (dB)", fontsize=8.5)
    # metrics text
    txt = []
    for n in ("G2", "D3", "A3"):
        txt.append("%s: lev50 %.1f dB (c3 %.1f, Phil %.1f), t_per %.2f s (c3 %.2f, Phil %.2f)" % (n, np.median(A4[n][:, 2]), np.median(A3[n][:, 2]), np.median(P[n][:, 2]), np.median(A4[n][:, 0]), np.median(A3[n][:, 0]), P[n][0, 0]))
    fig.text(0.055, 0.045, "   |   ".join(txt), fontsize=9)
    fig.text(0.055, 0.018, "Release bump (level rise after the lift, G2/D3/A3): see legend of the release panel. Wedge: 1.5 m/s^2 on all strings; force C 1.0, G 0.9, D and A 1.3 N; held 0.1 s, relaxed over 0.05-0.15 s. Seeds 0 1 2.", fontsize=9)
    fig.savefig(OUTPNG, dpi=100)
    print(OUTPNG)


main()
