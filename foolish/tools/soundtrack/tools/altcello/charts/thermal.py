"""ALT-CELLO thermal elasto-plastic bow friction chart, iteration 1 -> /Users/alex/Music/foolish_cities_v5/alt_cello/charts/thermal_iter1.png (about 1600 x 1100).

    python -I charts/thermal.py   (needs out/thermal/*: charts/thermal_run.sh BAND GRID REV, thermal_run2.sh curve / thermal, thermal_tool dumps made below)"""
import os, subprocess, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(HERE)
import onset_lib as L  # noqa: E402
import thermal_summary as S  # noqa: E402
G, M = L.G, L.M
SR = L.SR
D = os.path.join(ROOT, "out", "thermal")
OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/thermal_iter1.png"
CC, CT = "#3b6ea5", "#e07a1f"   # curve, thermal


def tool(args, knobs):
    env = dict(os.environ); env["ALTCELLO_KNOBS"] = knobs
    return subprocess.run(["nice", os.path.join(ROOT, "thermal_tool")] + args, capture_output=True, text=True, check=True, env=env, cwd=ROOT).stdout


def run_note(tag, string, F, secs, last, stride, curve):
    p = os.path.join(D, "note_%s.txt" % tag)
    cmd = ["note", string, "0.1", str(F), "0.1", str(secs), p, "--last", str(last), "--stride", str(stride)] + (["--curve"] if curve else [])
    out = tool(cmd, "bow=1")
    return np.loadtxt(p, skiprows=1), out.strip()


def gmap(ax, tag, case, title, first):
    p = os.path.join(D, "grid_%s_%s.txt" % (tag, case))
    d = np.array([l.split() for l in open(p) if not l.startswith("#")], float)
    Fs, As = sorted(set(d[:, 0])), sorted(set(d[:, 1]))
    T = np.full((len(Fs), len(As)), np.nan)
    for r in d:
        if r[6] > 0 and r[2] >= 0: T[Fs.index(r[0]), As.index(r[1])] = r[2]
    Fs, As = np.array(Fs), np.array(As)
    ae = np.concatenate([As / np.sqrt(As[1] / As[0]), [As[-1] * np.sqrt(As[1] / As[0])]])
    fe = np.concatenate([Fs / np.sqrt(Fs[1] / Fs[0]), [Fs[-1] * np.sqrt(Fs[1] / Fs[0])]])
    ax.set_facecolor("#888888")
    im = ax.pcolormesh(ae, fe, T, cmap="viridis_r", vmin=0.0, vmax=0.5, shading="flat")
    for i in range(len(Fs)):
        for j in range(len(As)):
            if np.isfinite(T[i, j]) and T[i, j] <= 0.1: ax.plot(As[j], Fs[i], "o", ms=3.5, mfc="none", mec="w", mew=1.1)
    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_title("%s\n%d cells <= 0.1 s" % (title, int(np.nansum(T <= 0.1))), fontsize=8.5)
    ax.tick_params(labelsize=7)
    if first: ax.set_ylabel("bow force F, N", fontsize=8)
    ax.set_xlabel("acceleration, m/s$^2$", fontsize=7)
    return im


def main():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    fig = plt.figure(figsize=(16, 11), dpi=100)
    fig.suptitle("ALT-CELLO - thermal elasto-plastic bow friction (van Walstijn et al. 2026) vs the friction curve - iteration 1", fontsize=17, fontweight="bold", y=0.985)
    gs = fig.add_gridspec(3, 12, left=0.05, right=0.985, top=0.925, bottom=0.06, hspace=0.46, wspace=0.95)

    # 1. hysteresis loops (D3, beta 0.1, F 0.67 N, v_B 0.1 m/s, string alone, the last 3 periods)
    ax = fig.add_subplot(gs[0, 0:4])
    for curve, col, nm in ((True, CC, "friction curve"), (False, CT, "thermal elasto-plastic")):
        d, _ = run_note("hys_%d" % curve, "D3", 0.67, 3.0, 3, 1, curve)
        ax.plot(d[:, 1], d[:, 2], color=col, lw=1.1, label=nm)
    v = np.linspace(0.0, 1.6, 200)
    ax.set_xlabel("relative velocity, m/s", fontsize=8); ax.set_ylabel("friction force / bow force", fontsize=8)
    ax.set_title("D3, three periods of Helmholtz motion: the curve is single valued, the thermal loop has hysteresis", fontsize=8.5)
    ax.legend(fontsize=7, loc="lower right"); ax.tick_params(labelsize=7); ax.axhline(0, color="gray", lw=0.4); ax.axvline(0, color="gray", lw=0.4)

    # 2. contact temperature over a note
    ax = fig.add_subplot(gs[0, 4:8])
    pk = {}
    for s, F, col in (("D3", 0.67, CT), ("C2", 1.2, "#7b3294")):
        d, out = run_note("tau_%s" % s, s, F, 1.0, 100000, 4, False)
        ax.plot(d[:, 0], 20.0 + d[:, 3], color=col, lw=0.6, label="%s, F %.2f N" % (s, F))
        pk[s] = float(np.max(20.0 + d[int(0.6 * len(d)):, 3]))
    ax.axhline(45.0, color="gray", ls="--", lw=0.8); ax.text(1.0, 45.8, "glass transition 45 C", fontsize=7, color="gray")
    ax.axhline(20.0, color="gray", lw=0.4)
    ax.set_xlabel("ms (1 s note, 4 ms shown stride)", fontsize=8); ax.set_ylabel("contact temperature, C (ambient 20)", fontsize=8)
    ax.set_title("contact temperature: pulses of heat in each slip (peak D3 %.0f C, C2 %.0f C; vW26: 66 C)" % (pk["D3"], pk["C2"]), fontsize=8.5)
    ax.legend(fontsize=7, loc="upper left"); ax.tick_params(labelsize=7)

    # 3. cells at or under 0.1 s and reversal times
    ax = fig.add_subplot(gs[0, 8:12])
    cases = [("C2 open", "C2_0"), ("G2 on C", "C2_7"), ("D3 on G", "G2_7"), ("A3 on D", "D3_7")]
    x = np.arange(4); w = 0.38
    for k, (tag, col, nm) in enumerate((("curve", CC, "curve"), ("thermal", CT, "thermal"))):
        v = [S.grid(tag, c)["fast"] for _, c in cases]
        ax.bar(x + (k - 0.5) * w, v, w, color=col, label=nm)
        for xi, vi in zip(x, v): ax.text(xi + (k - 0.5) * w, vi + 0.5, str(vi), ha="center", fontsize=7)
    ax.set_xticks(x); ax.set_xticklabels([c for c, _ in cases], fontsize=8)
    ax.set_ylabel("Guettler cells with time to\nperiodic <= 0.1 s (of 72)", fontsize=8); ax.legend(fontsize=7); ax.tick_params(labelsize=7)
    ax.set_title("Guettler cells at or under 0.1 s", fontsize=8.5)

    # 4. Guettler maps
    im = None
    for k, (tag, case, nm) in enumerate((("curve", "C2_7", "G2 on C, curve"), ("thermal", "C2_7", "G2 on C, thermal"), ("curve", "G2_7", "D3 on G, curve"), ("thermal", "G2_7", "D3 on G, thermal"))):
        ax = fig.add_subplot(gs[1, [0, 2, 4, 6][k]:[0, 2, 4, 6][k] + 2])
        im = gmap(ax, tag, case, nm, k % 2 == 0)
    cax = fig.add_axes([0.075, 0.405, 0.30, 0.007])
    cb = fig.colorbar(im, cax=cax, orientation="horizontal"); cb.set_label("audio time to periodic, s (gray: no steady Helmholtz; white ring: <= 0.1 s)", fontsize=7); cb.ax.tick_params(labelsize=7)

    # 5. bars: reversal, band, scale, HNR
    ax = fig.add_subplot(gs[1, 8:10])
    rv = {t: S.rev(t) for t in ("curve", "thermal")}
    for k, (tag, col) in enumerate((("curve", CC), ("thermal", CT))):
        v = [np.median(rv[tag]["G2"]), np.median(rv[tag]["C2"])]
        ax.bar(np.arange(2) + (k - 0.5) * w, v, w, color=col)
        for xi, vi in zip(range(2), v): ax.text(xi + (k - 0.5) * w, vi + 0.005, "%.2f" % vi, ha="center", fontsize=7)
    ax.set_xticks([0, 1]); ax.set_xticklabels(["D3 on G", "G2 on C"], fontsize=8); ax.set_ylabel("s, median of 3", fontsize=8); ax.tick_params(labelsize=7)
    ax.set_title("time to periodic after\na bow reversal", fontsize=8.5)
    ax = fig.add_subplot(gs[1, 10:12])
    bc = [("C2", "C2_0"), ("G2/C", "C2_7"), ("D3", "D3_0"), ("A3", "A3_0")]
    for k, (tag, col) in enumerate((("curve", CC), ("thermal", CT))):
        v = [S.band(tag, c)["ratio"] for _, c in bc]
        ax.bar(np.arange(4) + (k - 0.5) * w, v, w, color=col)
        for xi, vi in zip(range(4), v): ax.text(xi + (k - 0.5) * w, vi + 0.2, "%.1f" % vi, ha="center", fontsize=6.5)
    ax.axhline(3, color="k", lw=0.7, ls=":"); ax.set_xticks(range(4)); ax.set_xticklabels([c for c, _ in bc], fontsize=7.5); ax.tick_params(labelsize=7)
    ax.set_ylabel("contiguous Helmholtz band, F max / F min", fontsize=7.5); ax.set_title("bow force band\n(beta 0.1, v_B 0.1)", fontsize=8.5)

    # 6. D3 expert note: first 0.6 s level and pitch
    axl = fig.add_subplot(gs[2, 0:4]); axp = fig.add_subplot(gs[2, 4:8])
    yp = M.load_audio(G.PHIL_FULL % 50)
    ys = {}
    for tag, knobs in (("curve", "bow=0"), ("thermal", "bow=1")):
        w_ = os.path.join(L.TMP, "th_chart_%s.wav" % tag)
        env = dict(os.environ); env["ALTCELLO_KNOBS"] = knobs; os.environ["ALTCELLO_KNOBS"] = knobs
        L.render("single_notes.txt", w_, ["--att", "on", "--seed", "0"])
        ys[tag] = M.load_audio(w_)
    for y, nm, col, t0 in ((yp, "Philharmonia D3 (target)", "k", None), (ys["curve"], "friction curve", CC, 3.5), (ys["thermal"], "thermal", CT, 3.5)):
        seg = y if t0 is None else y[int((t0 - 0.2) * SR):int((t0 + 2.3) * SR)]
        on = G.onset_s(seg) if t0 is None else 0.2
        env = 20 * np.log10(M.rms_env(seg) + 1e-12); a = int(on * 100)
        t = (np.arange(a, a + 62) - a) * 0.01
        axl.plot(t, env[a:a + 62] - np.max(env[a:a + 150]), color=col, lw=1.8, label=nm)
        ts, c = G.acpitch(seg, M.midi_hz(50), on, on + 0.6, hop=0.01)
        axp.plot(ts - on, c, color=col, lw=1.6, label=nm)
    axl.set_xlim(0, 0.6); axl.set_ylim(-45, 3); axl.set_ylabel("level (10 ms rms), dB re peak", fontsize=8); axl.legend(fontsize=7)
    axl.set_title("expert D3, first 0.6 s: level", fontsize=8.5); axl.tick_params(labelsize=7); axl.set_xlabel("s after the onset", fontsize=8)
    axp.set_xlim(0, 0.6); axp.set_ylim(-80, 60); axp.axhline(0, color="gray", lw=0.5); axp.set_ylabel("pitch, cents re 146.83 Hz", fontsize=8); axp.set_xlabel("s after the onset", fontsize=8)
    axp.set_title("expert D3, first 0.6 s: pitch", fontsize=8.5); axp.tick_params(labelsize=7)

    # 7. metrics
    ax = fig.add_subplot(gs[2, 8:12]); ax.axis("off")
    g = {t: S.gates(t) for t in ("curve", "thermal")}
    ph = S.gates_phil()
    def med(t, lab, k): return float(np.median([r[S.GKEYS.index(k)] for r in g[t][lab]]))
    sc = {}
    for t in ("curve", "thermal"):
        for l in open(os.path.join(ROOT, "out", "ring_study", "th_%s.txt" % t)):
            if " scale " in l: sc[t] = eval(l.split(" scale ", 1)[1])
    lines = ["METRICS (curve | thermal; Philharmonia where measured)", "",
             "expert notes, medians of 3 seeds (dry)",
             "              C2    G2/C   G2    D3    A3",
             "t periodic s  " + "  ".join("%.2f" % x for x in [med("curve", l, "tper") for l in ("36", "43c", "43", "50", "57")]),
             "  thermal     " + "  ".join("%.2f" % x for x in [med("thermal", l, "tper") for l in ("36", "43c", "43", "50", "57")]),
             "HNR dB curve  " + "  ".join("%.1f" % x for x in [med("curve", l, "hnr") for l in ("36", "43c", "43", "50", "57")]),
             "  thermal     " + "  ".join("%.1f" % x for x in [med("thermal", l, "hnr") for l in ("36", "43c", "43", "50", "57")]),
             "1-3k share cu " + "  ".join("%.1f" % x for x in [med("curve", l, "share13") for l in ("36", "43c", "43", "50", "57")]),
             "  thermal     " + "  ".join("%.1f" % x for x in [med("thermal", l, "share13") for l in ("36", "43c", "43", "50", "57")]),
             "  Phil        " + "  ".join("%.1f" % ph[l][0][S.GKEYS.index("share13")] for l in ("36", "43c", "43", "50", "57")),
             "2-6k att-sus c" + "  ".join("%.1f" % x for x in [med("curve", l, "att_tilt") for l in ("36", "43c", "43", "50", "57")]),
             "  thermal     " + "  ".join("%.1f" % x for x in [med("thermal", l, "att_tilt") for l in ("36", "43c", "43", "50", "57")]),
             "", "scale note changes (21): median / max ms",
             "  curve %.0f / %.0f   thermal %.0f / %.0f" % (sc["curve"]["median"], sc["curve"]["max"], sc["thermal"]["median"], sc["thermal"]["max"]),
             "", "real-time factor (wall / sim time, full instrument):", open(os.path.join(D, "rtf.txt")).read().strip() if os.path.exists(os.path.join(D, "rtf.txt")) else "-"]
    ax.text(0.0, 1.0, "\n".join(lines), va="top", ha="left", family="monospace", fontsize=7.3)
    fig.savefig(OUT, dpi=100)
    print(OUT)


main()
