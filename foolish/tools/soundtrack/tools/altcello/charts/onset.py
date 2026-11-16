"""ALT-CELLO onset and legato chart, iteration 1 -> charts/onset_iter1.png (about 1600 x 1100).

    python -I charts/onset.py        (needs out/onset/grid_*.txt, fing_*.txt, gates2.txt, cand2/*.wav: onset_tool grid, finger_study.py, gates2.py, candidate2.py)"""
import os, sys, subprocess, re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LogNorm
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import onset_lib as L  # noqa: E402
import transitions as TR  # noqa: E402
G, M = L.G, L.M
SR = L.SR
OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/onset_iter1.png"
GR = os.path.join(L.TMP)


def grid(f):
    rows = [l.split() for l in open(f) if not l.startswith("#")]
    d = np.array(rows, float)
    Fs, As = sorted(set(d[:, 0])), sorted(set(d[:, 1]))
    T = np.full((len(Fs), len(As)), np.nan)
    ok = np.zeros((len(Fs), len(As)))
    for r in d:
        i, j = Fs.index(r[0]), As.index(r[1])
        T[i, j] = r[2]
        ok[i, j] = r[6]
    return np.array(Fs), np.array(As), T, ok


def gmap(ax, f, title):
    Fs, As, T, ok = grid(f)
    A = np.where((ok > 0) & (T >= 0), T, np.nan)
    ae = np.concatenate([As / np.sqrt(As[1] / As[0]), [As[-1] * np.sqrt(As[1] / As[0])]])
    fe = np.concatenate([Fs / np.sqrt(Fs[1] / Fs[0]), [Fs[-1] * np.sqrt(Fs[1] / Fs[0])]])
    ax.set_facecolor("#888888")
    im = ax.pcolormesh(ae, fe, A, cmap="viridis_r", vmin=0.0, vmax=0.5, shading="flat")
    for i in range(len(Fs)):
        for j in range(len(As)):
            if np.isfinite(A[i, j]) and A[i, j] <= 0.1:
                ax.plot(As[j], Fs[i], "o", ms=4, mfc="none", mec="w", mew=1.2)
    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_title(title, fontsize=9)
    ax.set_xlabel("bow acceleration a, m/s$^2$", fontsize=8)
    ax.tick_params(labelsize=7)
    n80 = int(np.sum(A <= 0.08)); n100 = int(np.sum(A <= 0.1))
    ax.text(0.03, 0.04, "cells <= 80 ms: %d, <= 100 ms: %d of %d" % (n80, n100, A.size), transform=ax.transAxes, fontsize=7, color="w", bbox=dict(fc="k", alpha=0.5, lw=0))
    return im


def fing_rows(f):
    out = []
    for l in open(f):
        p = [x.strip() for x in l.split("|")]
        m = re.search(r"t_pitch (\S+)", p[3])
        out.append((p[1], float(m.group(1))))
    return out


def level_pitch(y, midi, on):
    env = 20 * np.log10(M.rms_env(y) + 1e-12)
    a = int(on * 100)
    t = (np.arange(a, a + 62) - a) * 0.01 - 0.0
    lev = env[a:a + 62] - np.max(env[a:a + 150])
    ts, c = G.acpitch(y, M.midi_hz(midi), on, on + 0.6, hop=0.01)
    return t, lev, ts - on, c


def main():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    fig = plt.figure(figsize=(16, 11), dpi=100)
    fig.suptitle("ALT-CELLO - onset and legato: Guettler diagrams, finger stop and expert attack - iteration 1", fontsize=17, fontweight="bold", y=0.985)
    gs = fig.add_gridspec(3, 5, left=0.05, right=0.985, top=0.925, bottom=0.06, hspace=0.42, wspace=0.28)
    names = [("grid_ours_D3on_G.txt", "ours D3 (G string, F $\\times$ 2 band)"), ("grid_ours_A3on_D.txt", "ours A3 (D string)"), ("grid_ours_G2on_C.txt", "ours G2 (C string)"),
             ("grid_ours_D3open.txt", "ours open D"), ("grid_oracle_D3open.txt", "MSW oracle, open D")]
    im = None
    for k, (f, t) in enumerate(names):
        ax = fig.add_subplot(gs[0, k])
        im = gmap(ax, os.path.join(GR, f), t)
        if k == 0: ax.set_ylabel("bow force F, N", fontsize=8)
    cax = fig.add_axes([0.32, 0.655, 0.35, 0.008])
    cb = fig.colorbar(im, cax=cax, orientation="horizontal")
    cb.set_label("audio time to periodic, s (gray: no steady Helmholtz motion; white ring: <= 0.1 s)", fontsize=7)
    cb.ax.tick_params(labelsize=7)
    # finger bars
    ax = fig.add_subplot(gs[1, 0:2])
    sets = [("slur 8 to 10 semis on G (no open-string unison)", "fing_slur_G2_8_10.txt", "#2a9d8f"), ("slur 7 to 9 on G (D3 to E3, D3 = open D)", "fing_slur_G2.txt", "#e76f51"),
            ("lift E3 to open D3", "fing_lift_D3.txt", "#6d597a")]
    labs = [r[0].replace(" (ramp 10 ms K1 R1 w10)", "") for r in fing_rows(os.path.join(GR, sets[0][1]))]
    w = 0.27
    for k, (nm, f, col) in enumerate(sets):
        r = fing_rows(os.path.join(GR, f))
        ax.bar(np.arange(len(r)) + (k - 1) * w, [x[1] * 1000 if x[1] >= 0 else np.nan for x in r], w, label=nm, color=col)
    ax.set_xticks(range(len(labs))); ax.set_xticklabels(labs, rotation=30, ha="right", fontsize=7)
    ax.set_ylabel("ms from the start of the finger event to the\nnew pitch (10 c, 5 periods), median of 3", fontsize=8)
    ax.set_title("finger parameters one at a time: no pad setting controls the delay; the open-string unison does", fontsize=9)
    ax.set_ylim(0, 270); ax.legend(fontsize=7, loc="upper left"); ax.tick_params(labelsize=7)
    # D3 level and pitch
    c1 = os.path.join(L.TMP, "n3_c1.wav")
    L.render("single_notes.txt", c1, ["--att", "off", "--seed", "0"])
    y1 = M.load_audio(c1)
    y2 = M.load_audio(os.path.join(L.TMP, "cand2", "notes_s0_dry.wav"))
    yp = M.load_audio(G.PHIL_FULL % 50)
    axl = fig.add_subplot(gs[1, 2:4]); axp = fig.add_subplot(gs[2, 2:4])
    for y, nm, col, t0 in ((yp, "Philharmonia D3 (target)", "k", None), (y1, "candidate 1", "#e76f51", 3.5), (y2, "candidate 2", "#2a9d8f", 3.5)):
        seg = y if t0 is None else y[int((t0 - 0.2) * SR):int((t0 + 2.3) * SR)]
        on = G.onset_s(seg) if t0 is None else 0.2  # ours: the written start = the start of the bow motion; the recording: its detected onset
        t, lev, tc, c = level_pitch(seg, 50, on)
        axl.plot(t, lev, color=col, label=nm, lw=1.8)
        axp.plot(tc, c, color=col, label=nm, lw=1.6)
    axl.set_ylim(-45, 3); axl.set_xlim(0, 0.6); axl.set_ylabel("level (10 ms rms), dB re peak", fontsize=8); axl.set_title("D3, first 0.6 s (ours from the start of the bow motion, the recording from its onset)", fontsize=9); axl.legend(fontsize=7)
    axp.set_xlim(0, 0.6); axp.set_ylim(-80, 60); axp.set_xlabel("s after the onset", fontsize=8); axp.set_ylabel("pitch, cents re 146.83 Hz", fontsize=8); axp.axhline(0, color="gray", lw=0.5)
    axl.tick_params(labelsize=7); axp.tick_params(labelsize=7)
    # scale pitch track, vibrato off half
    ax = fig.add_subplot(gs[2, 0:2])
    nts = [n for n in TR.notes_of(os.path.join(L.SC, "c_major_scale.txt")) if n[3] == 0]
    for wav, nm, col in ((os.path.join(L.TMP, "scale_c1_dry.wav"), "candidate 1", "#e76f51"), (os.path.join(L.TMP, "cand2", "scale_dry.wav"), "candidate 2", "#2a9d8f")):
        y = M.load_audio(wav)
        t, f = TR.track(y, 100.0, 330.0, 0.012, nts[0][0] - 0.1, nts[-1][0] + nts[-1][1] + 0.1, "chart")
        m = np.where(f > 0, 12 * np.log2(np.maximum(f, 1) / 440.0) + 69, np.nan)
        st = np.array([n[0] for n in nts]); mid = np.array([n[2] for n in nts])
        ix = np.clip(np.searchsorted(st, t + 0.02, side="right") - 1, 0, len(nts) - 1)
        sc, prev = mid[ix], mid[np.clip(ix - 1, 0, len(nts) - 1)]
        keep = (np.abs(m - sc) < 1.5) | (np.abs(m - prev) < 1.5)
        ax.plot((t - nts[0][0])[keep], m[keep], ".", ms=1.6, color=col, label=nm)
    for ts, dur, midi, _ in nts: ax.plot([ts - nts[0][0], ts - nts[0][0] + dur], [midi, midi], color="k", lw=1.2)
    ax.set_ylim(46, 62); ax.legend(markerscale=5, fontsize=7, loc="upper left"); ax.set_xlabel("s (vibrato-off scale; black = the score; dots within 1.5 semitones of the note or the one before)", fontsize=7); ax.set_ylabel("pitch, MIDI", fontsize=8)
    ax.set_title("scale pitch track: note changes arrive late and via the old pitch", fontsize=9); ax.tick_params(labelsize=7)
    # metrics
    ax = fig.add_subplot(gs[1:3, 4]); ax.axis("off")
    gt = {}
    for l in open(os.path.join(GR, "gates2.txt")):
        if l.startswith("ROW"):
            p = l.split(); gt.setdefault((p[1], int(p[2])), []).append([float(x) for x in p[4:]])
    def med(s, m, k): return np.nanmedian([r[k] for r in gt[(s, m)]])
    lines = ["METRICS (dry, medians of 3 takes)", "", "audio time to periodic, s", "       G2    D3    A3    C2"]
    for s, nm in (("c1", "cand 1"), ("c2", "cand 2")):
        lines.append("%s %5.2f %5.2f %5.2f %5.2f" % (nm, *[med(s, m, 0) if med(s, m, 0) > 0 else float("nan") for m in (43, 50, 57, 36)]))
    lines.append("Phil %5.2f %5.2f %5.2f %5.2f (D2)" % tuple(med("phil", m, 0) for m in (43, 50, 57, 38)))
    lines += ["", "attack 10-90 ms", "       G2    D3    A3"]
    for s, nm in (("c1", "cand 1"), ("c2", "cand 2")):
        lines.append("%s %5.0f %5.0f %5.0f" % (nm, med(s, 43, 1), med(s, 50, 1), med(s, 57, 1)))
    lines.append("Phil %5.0f %5.0f %5.0f" % tuple(med("phil", m, 1) for m in (43, 50, 57)))
    lines += ["", "scale note changes (vib off, 7):", "new pitch within 10 c, median:", "  cand 1 206 ms, cand 2 175 ms", "none within 30 ms; median dip", "  -259 / -148 c (the old pitch", "  keeps ringing, see left)", "", "Guettler (ours, v_end 0.1):", "  cells <= 100 ms in the grid:", "  A3 13, D3 0, G2 0, open D 0-1", "  (v_end 0.05: A3 27, D3 7, G2 0)", "", "finger pad K, R, width, ramp:", "  no effect beyond take noise", "bow change D3 on G: 0.14-0.48 s"]
    ax.text(0.0, 1.0, "\n".join(lines), va="top", ha="left", family="monospace", fontsize=7.4)
    fig.savefig(OUT, dpi=100)
    print(OUT)


main()
