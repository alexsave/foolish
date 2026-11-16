"""ALT-CELLO thermal elasto-plastic bow friction chart, iteration 1 -> /Users/alex/Music/foolish_cities_v5/alt_cello/charts/thermal_iter1.png (about 1600 x 1100).

    python -I charts/thermal.py
Needs out/thermal/ (charts/thermal_run.sh BAND GRID REV for the models curve and final, thermal_run2.sh curve / final2, rtf.txt), out/ring_study/th_*.txt and the candidate 2 and 3
folders (charts/candidate3.py). 'curve' = the friction curve with the candidate 2 player; 'thermal' = THERMAL with the regenerated playability table and the attack at 0.4."""
import os, subprocess, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter, FormatStrFormatter
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(HERE)
import onset_lib as L  # noqa: E402
import thermal_summary as S  # noqa: E402
G, M = L.G, L.M
SR = L.SR
D = os.path.join(ROOT, "out", "thermal")
OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/thermal_iter1.png"
AUD = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio"
CC, CT, CP = "#3b6ea5", "#e07a1f", "k"   # curve, thermal, Philharmonia
TH = "final"        # the model tag of the thermal runs (charts/thermal_run.sh)
THG = "final2"      # the tag of the expert note runs


def tool(args):
    env = dict(os.environ); env["ALTCELLO_KNOBS"] = "bow=1"
    return subprocess.run(["nice", os.path.join(ROOT, "thermal_tool")] + args, capture_output=True, text=True, check=True, env=env, cwd=ROOT).stdout


def run_note(tag, string, F, secs, last, stride, curve):
    p = os.path.join(D, "note_%s.txt" % tag)
    tool(["note", string, "0.1", str(F), "0.1", str(secs), p, "--last", str(last), "--stride", str(stride)] + (["--curve"] if curve else []))
    return np.loadtxt(p, skiprows=1)


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
    for a in (ax.xaxis, ax.yaxis):
        a.set_major_formatter(FormatStrFormatter("%g")); a.set_minor_formatter(plt.NullFormatter())
    ax.set_xticks([0.25, 1, 4]); ax.set_yticks([t for t in (0.1, 0.3, 1, 3) if fe[0] <= t <= fe[-1]])
    ax.set_title("%s: %d cells <= 0.1 s" % (title, int(np.nansum(T <= 0.1))), fontsize=8.5)
    ax.tick_params(labelsize=7)
    if first: ax.set_ylabel("bow force F, N", fontsize=8)
    ax.set_xlabel("bow acceleration, m/s$^2$", fontsize=7.5)
    return im


def bars(ax, groups, vals, colors, labels, fmt="%.2f", dy=0.0, ylab="", title="", phil=None):
    n = len(vals); w = 0.8 / n
    for k in range(n):
        x = np.arange(len(groups)) + (k - (n - 1) / 2) * w
        ax.bar(x, vals[k], w, color=colors[k], label=labels[k])
        for xi, vi in zip(x, vals[k]):
            if np.isfinite(vi): ax.text(xi, vi + dy, fmt % vi, ha="center", va="bottom", fontsize=6.3)
    if phil is not None:
        ax.plot(np.arange(len(groups)), phil, "k_", ms=18, mew=2, label="Philharmonia")
    ax.set_xticks(range(len(groups))); ax.set_xticklabels(groups, fontsize=7.5); ax.tick_params(labelsize=7)
    ax.set_ylabel(ylab, fontsize=7.5); ax.set_title(title, fontsize=8.5)


def main():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    fig = plt.figure(figsize=(16, 11), dpi=100)
    fig.suptitle("ALT-CELLO - thermal elasto-plastic bow friction (van Walstijn et al. 2026) vs the friction curve - iteration 1", fontsize=17, fontweight="bold", y=0.985)
    gs = fig.add_gridspec(4, 12, left=0.05, right=0.955, top=0.925, bottom=0.05, hspace=0.62, wspace=1.0, height_ratios=[1.0, 1.0, 0.9, 1.0])

    # 1. hysteresis loops: D3, beta 0.1, F 0.67 N, v_B 0.1 m/s, string alone, the last 3 periods (sign flipped: slip speed and friction coefficient, positive)
    ax = fig.add_subplot(gs[0, 0:4])
    for curve, col, nm in ((True, CC, "friction curve"), (False, CT, "thermal elasto-plastic")):
        d = run_note("hys_%d" % curve, "D3", 0.67, 3.0, 3, 1, curve)
        ax.plot(-d[:, 1], -d[:, 2], color=col, lw=1.1, label=nm)
    ax.set_xlabel("slip speed, m/s", fontsize=8); ax.set_ylabel("friction force / bow force", fontsize=8)
    ax.set_title("D3, 3 periods of Helmholtz motion: friction vs speed", fontsize=8.5)
    ax.legend(fontsize=7, loc="upper right"); ax.tick_params(labelsize=7); ax.set_xlim(-0.05, 1.5); ax.set_ylim(-0.05, 1.15)

    # 2. contact temperature over a note: peak of each period and a zoom of 3 periods
    sub = gs[0, 4:8].subgridspec(2, 1, height_ratios=[1.1, 1.0], hspace=0.55)
    ax = fig.add_subplot(sub[0]); az = fig.add_subplot(sub[1])
    pk = {}
    for s, F, col in (("D3", 0.67, CT), ("C2", 1.2, "#7b3294")):
        d = run_note("tau_%s" % s, s, F, 1.0, 100000, 2, False)
        P = int(round(96000.0 / (2 * G.M.midi_hz(50 if s == "D3" else 36))))  # samples per period at stride 2
        T = 20.0 + d[:, 3]
        n = len(T) // P
        ax.plot(d[:n * P:P, 0][:n] + 0.0, T[:n * P].reshape(n, P).max(axis=1), color=col, lw=1.2, label="%s, F %.2f N (peak of each period)" % (s, F))
        pk[s] = float(np.max(T[int(0.6 * len(T)):]))
        if s == "D3":
            i0 = int(0.8 * len(T)); az.plot(d[i0:i0 + 3 * P, 0] - d[i0, 0], T[i0:i0 + 3 * P], color=col, lw=1.2)
    ax.axhline(45.0, color="gray", ls="--", lw=0.8); ax.text(15, 47.0, "glass transition 45 C", fontsize=6.5, color="gray", ha="left")
    ax.set_ylabel("contact temperature, C", fontsize=7.5); ax.set_xlabel("ms (ambient 20 C)", fontsize=7.5); ax.tick_params(labelsize=7); ax.legend(fontsize=6.3, loc="lower right")
    ax.set_title("contact temperature over a note: peaks D3 %.0f C, C2 %.0f C (vW26: 66 C)" % (pk["D3"], pk["C2"]), fontsize=8.2)
    az.set_ylabel("C", fontsize=7.5); az.set_xlabel("ms, three periods of D3: heat in each slip, cooling in each stick", fontsize=7); az.tick_params(labelsize=7)

    # 3. Guettler cells
    ax = fig.add_subplot(gs[0, 8:12])
    cases = [("C2 open", "C2_0"), ("G2 on C", "C2_7"), ("D3 on G", "G2_7"), ("A3 on D", "D3_7")]
    bars(ax, [c for c, _ in cases], [[S.grid(t, c)["fast"] for _, c in cases] for t in ("curve", TH)], [CC, CT], ["curve", "thermal"], fmt="%d", dy=0.4,
         ylab="cells with time to periodic <= 0.1 s (of 72)", title="Guettler cells at or under 0.1 s")
    ax.legend(fontsize=7, loc="upper left")

    # 4. Guettler maps
    im = None
    sg = gs[1, :].subgridspec(1, 5, width_ratios=[1, 1, 1, 1, 0.05], wspace=0.28)
    for k, (tag, case, nm) in enumerate((("curve", "C2_7", "G2 on C, curve"), (TH, "C2_7", "G2 on C, thermal"), ("curve", "G2_7", "D3 on G, curve"), (TH, "G2_7", "D3 on G, thermal"))):
        ax = fig.add_subplot(sg[0, k])
        im = gmap(ax, tag, case, nm, k % 2 == 0)
    cax = fig.add_subplot(sg[0, 4])
    cb = fig.colorbar(im, cax=cax); cb.set_label("audio time to periodic, s\n(gray: no steady Helmholtz; white ring: <= 0.1 s)", fontsize=7); cb.ax.tick_params(labelsize=7)

    # 5. bars: reversal, band, expert-note time to periodic, HNR, scale
    rv = {t: S.rev(t) for t in ("curve", TH)}
    ax = fig.add_subplot(gs[2, 0:2])
    bars(ax, ["D3 on G", "G2 on C"], [[np.median(rv[t]["G2"]), np.median(rv[t]["C2"])] for t in ("curve", TH)], [CC, CT], ["curve", "thermal"], dy=0.004, ylab="s, median of 3", title="time to periodic\nafter a bow reversal")
    ax = fig.add_subplot(gs[2, 2:4])
    bc = [("C2", "C2_0"), ("G2/C", "C2_7"), ("D3", "D3_0"), ("A3", "A3_0")]
    bars(ax, [c for c, _ in bc], [[S.band(t, c)["ratio"] for _, c in bc] for t in ("curve", "thermal")], [CC, CT], ["curve", "thermal"], fmt="%.1f", dy=0.15, ylab="F max / F min, contiguous", title="Helmholtz band\n(beta 0.1, v_B 0.1)")
    ax.axhline(3, color="k", lw=0.7, ls=":")
    g = {t: S.gates(t) for t in ("curve", THG)}
    ph = S.gates_phil()
    labs = [("36", "C2"), ("43c", "G2/C"), ("43", "G2"), ("50", "D3"), ("57", "A3")]
    def med(t, lab, k): return float(np.median([r[S.GKEYS.index(k)] for r in g[t][lab]]))
    ax = fig.add_subplot(gs[2, 4:7])
    tp = [[min(med(t, l, "tper"), 1.0) if med(t, l, "tper") >= 0 else 1.0 for l, _ in labs] for t in ("curve", THG)]
    bars(ax, [n for _, n in labs], tp, [CC, CT], ["curve", "thermal"], dy=0.01, ylab="s (1.0 = never)", title="expert notes: audio time to periodic\n(median of 3; Philharmonia dots)",
         phil=[min(max(ph[l][0][S.GKEYS.index("tper")], 0.0), 1.0) for l, _ in labs])
    ax = fig.add_subplot(gs[2, 7:10])
    bars(ax, [n for _, n in labs], [[med(t, l, "hnr") for l, _ in labs] for t in ("curve", THG)], [CC, CT], ["curve", "thermal"], fmt="%.0f", dy=0.3, ylab="dB", title="expert notes: HNR\n(median of 3; Philharmonia dots)",
         phil=[ph[l][0][S.GKEYS.index("hnr")] for l, _ in labs])
    sc = {}
    for t in ("curve", THG):
        for l in open(os.path.join(ROOT, "out", "ring_study", "th_%s.txt" % t)):
            if " scale " in l: sc[t] = eval(l.split(" scale ", 1)[1])
    ax = fig.add_subplot(gs[2, 10:12])
    bars(ax, ["median", "max"], [[sc[t]["median"], sc[t]["max"]] for t in ("curve", THG)], [CC, CT], ["curve", "thermal"], fmt="%.0f", dy=4, ylab="ms", title="scale: new pitch within\n10 c (21 changes)")

    # 6. D3 expert note, first 0.6 s: candidate files (the dry notes of the two candidates against the Philharmonia D3)
    axl = fig.add_subplot(gs[3, 0:4]); axp = fig.add_subplot(gs[3, 4:8])
    yp = M.load_audio(G.PHIL_FULL % 50)
    c2 = M.load_audio(os.path.join(AUD, "candidate2", "notes_alt_dry.wav")); c3 = M.load_audio(os.path.join(AUD, "candidate3", "notes_alt_dry.wav"))
    for y, nm, col, t0 in ((yp, "Philharmonia D3 (target)", CP, None), (c2, "candidate 2 (friction curve)", CC, 3.4), (c3, "candidate 3 (thermal)", CT, 3.4)):
        seg = y if t0 is None else y[int(t0 * SR):int((t0 + 2.4) * SR)]
        on = G.onset_s(seg)
        env = 20 * np.log10(M.rms_env(seg) + 1e-12); a = int(on * 100)
        axl.plot((np.arange(a, a + 62) - a) * 0.01, env[a:a + 62] - np.max(env[a:a + 150]), color=col, lw=1.8, label=nm)
        ts, c = G.acpitch(seg, M.midi_hz(50), on, on + 0.6, hop=0.01)
        axp.plot(ts - on, c, color=col, lw=1.6, label=nm)
    axl.set_xlim(0, 0.6); axl.set_ylim(-45, 3); axl.set_ylabel("level (10 ms rms), dB re peak", fontsize=8); axl.legend(fontsize=7, loc="lower right")
    axl.set_title("D3 expert note, first 0.6 s: level", fontsize=8.5); axl.tick_params(labelsize=7); axl.set_xlabel("s after the onset", fontsize=8)
    axp.set_xlim(0, 0.6); axp.set_ylim(-80, 60); axp.axhline(0, color="gray", lw=0.5); axp.set_ylabel("pitch, cents re 146.83 Hz", fontsize=8); axp.set_xlabel("s after the onset", fontsize=8)
    axp.set_title("D3 expert note, first 0.6 s: pitch", fontsize=8.5); axp.tick_params(labelsize=7)

    # 7. metrics
    ax = fig.add_subplot(gs[3, 8:12]); ax.axis("off")
    cols = [l for l, _ in labs]
    def row(name, f): return "%-10s" % name + "".join("%7s" % f(l) for l in cols)
    lines = ["METRICS (thermal = the new default; curve = candidate 2 state)", "",
             "expert notes, medians of 3 seeds, dry",
             row("", lambda l: dict(labs)[l]),
             row("t per cur", lambda l: "%.2f" % med("curve", l, "tper") if med("curve", l, "tper") >= 0 else "never"),
             row("t per th", lambda l: "%.2f" % med(THG, l, "tper")),
             row("HNR cur", lambda l: "%.1f" % med("curve", l, "hnr")),
             row("HNR th", lambda l: "%.1f" % med(THG, l, "hnr")),
             row("1-3k cur", lambda l: "%.1f" % med("curve", l, "share13")),
             row("1-3k th", lambda l: "%.1f" % med(THG, l, "share13")),
             row("1-3k Phil", lambda l: "%.1f" % ph[l][0][S.GKEYS.index("share13")]),
             row("2-6k cur", lambda l: "%.1f" % med("curve", l, "att_tilt")),
             row("2-6k th", lambda l: "%.1f" % med(THG, l, "att_tilt")),
             row("2-6k Phil", lambda l: "%.1f" % ph[l][0][S.GKEYS.index("att_tilt")]),
             "(1-3k: share of 50-16000 Hz, dB; 2-6k: attack minus sustain, dB)", "",
             "peak contact temperature D3 %.0f C (vW26 66 C, 21 K over tau_g)" % pk["D3"],
             "real-time factor, audio s per CPU s: " + open(os.path.join(D, "rtf.txt")).read().splitlines()[0].split(": ")[1]]
    ax.text(0.0, 1.0, "\n".join(lines), va="top", ha="left", family="monospace", fontsize=7.2)
    fig.savefig(OUT, dpi=100)
    print(OUT)


main()
