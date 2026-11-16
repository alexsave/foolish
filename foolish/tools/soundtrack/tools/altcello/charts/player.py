"""ALT-CELLO expert player chart -> alt_cello/charts/player_iter1.png. Run charts/candidate1.py first (it renders the dry notes and scale with traces).
    python -I charts/player.py
"""
import os, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.gridspec import GridSpecFromSubplotSpec as Sub
import scipy.signal as ss
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import player_gates as G  # noqa: E402
M = G.M
TMP = G.TMP
SR = 48000
OUTPNG = "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/player_iter1.png"
CE, CN, CT = "#0072B2", "#D55E00", "#333333"  # expert, novice, Philharmonia target (Okabe-Ito)
NOTES = [(43, 0.5), (50, 3.5), (57, 6.5)]


def ctrl(tag):
    return np.fromfile(os.path.join(TMP, tag + "_ctrl.f32"), dtype=np.float32).reshape(-1, 11)


def main():
    de, dn = ctrl("cand_notes_alt_dry"), ctrl("cand_notes_novice_dry")
    ye = M.load_audio(os.path.join(TMP, "cand_notes_alt_dry.wav"))
    yn = M.load_audio(os.path.join(TMP, "cand_notes_novice_dry.wav"))
    ysc = M.load_audio(os.path.join(TMP, "cand_scale_alt_dry.wav"))
    ge = {m: G.alt_note_gates(ye, t, m, "ce%d" % m) for m, t in NOTES}
    gn = {m: G.alt_note_gates(yn, t, m, "cn%d" % m) for m, t in NOTES}
    gr = {m: G.ref_gates(m) for m, _ in NOTES}
    me, mn, mr = G.median_table(list(ge.values())), G.median_table(list(gn.values())), G.median_table(list(gr.values()))

    fig = plt.figure(figsize=(16, 11), dpi=100)
    fig.suptitle("ALT-CELLO - expert player model: gestures and the result - single notes G2 D3 A3 and C major scale - iteration 1", fontsize=17, fontweight="bold", y=0.985)
    outer = fig.add_gridspec(2, 2, left=0.055, right=0.945, top=0.925, bottom=0.075, hspace=0.26, wspace=0.17)

    # (a) gestures of the D3 note, expert and novice
    sa = Sub(5, 1, subplot_spec=outer[0, 0], hspace=0.12)
    axs = [fig.add_subplot(sa[i]) for i in range(5)]
    t0 = 3.5
    i0, i1 = int((t0 - 0.2) * 1000), int((t0 + 1.9) * 1000)
    tt = np.arange(i0, i1) / 1000.0 - t0
    for d, c, lab in ((de, CE, "EXPERT"), (dn, CN, "NOVICE")):
        r = d[i0:i1]
        axs[0].plot(tt, np.abs(r[:, 0]), color=c, lw=1.5, label=lab)
        axs[1].plot(tt, r[:, 1], color=c, lw=1.5)
        axs[3].plot(tt, r[:, 3], color=c, lw=1.5)
        axs[4].plot(tt, r[:, 7], color=c, lw=1.5)
        axs[2].plot(tt, r[:, 5], color=c, lw=1.5)
    r = de[i0:i1]
    lo, hi = r[:, 8], r[:, 9]
    f20, f80 = np.exp(np.log(lo) + 0.2 * np.log(hi / lo)), np.exp(np.log(lo) + 0.8 * np.log(hi / lo))
    axs[1].fill_between(tt, f20, f80, color="#bbbbbb", alpha=0.5, lw=0, label="20-80 % of the log band (playability table)")
    axs[1].plot(tt, lo, color="#888888", lw=0.8, ls=":", label="F_min, F_max")
    axs[1].plot(tt, hi, color="#888888", lw=0.8, ls=":")
    axs[1].set_ylim(0, 4)
    for ax, lab in zip(axs, ["|v_B| m/s", "F_B N", "press", "beta", "finger, cents"]):
        ax.set_ylabel(lab, fontsize=9)
        ax.grid(alpha=0.25)
        ax.tick_params(labelsize=8)
    for ax in axs[:-1]: ax.set_xticklabels([])
    axs[4].set_xlabel("seconds from the written note start (D3, vel 0.85, vibrato on; played on the G string)", fontsize=9)
    axs[0].set_title("(a) gestures of one note, EXPERT vs NOVICE", fontsize=10.5, loc="left")
    axs[0].legend(fontsize=8, loc="upper right", ncol=2)
    axs[1].legend(fontsize=7, loc="upper right")

    # (b) pitch track of the scale
    ab = fig.add_subplot(outer[0, 1])
    notes = [l.split() for l in open("/Users/alex/Music/foolish_cities_v5/alt_cello/scores/c_major_scale.txt") if not l.startswith("#")]
    for k, n in enumerate(notes):
        t, d, m = float(n[0]), float(n[1]), int(n[2])
        ts, cc = G.acpitch(ysc, M.midi_hz(m), t, t + d - 0.13, 0.01)
        ab.plot(ts, cc, color=CE, lw=1.3)
        ab.text(t + 0.05, 62, "%d" % m, fontsize=7, color=CT)
    ab.axhline(0, color=CT, lw=0.8)
    ab.axhspan(-10, 10, color="#bbbbbb", alpha=0.35, lw=0)
    t_split = float(notes[8][0]) - 0.5
    ab.axvline(t_split, color=CT, ls="--", lw=1)
    ab.text(0.6, -95, "vibrato ON", fontsize=10, color=CT)
    ab.text(t_split + 0.2, -95, "vibrato OFF", fontsize=10, color=CT)
    ab.set_ylim(-100, 75)
    ab.set_xlabel("seconds", fontsize=9)
    ab.set_ylabel("pitch, cents re the target note (autocorrelation track)", fontsize=9)
    ab.set_title("(b) C major scale, 72 BPM legato: pitch (grey = +-10 c)", fontsize=10.5, loc="left")
    ab.grid(alpha=0.25)
    ab.tick_params(labelsize=8)
    ab.text(0.99, 0.84, "new notes arrive 0.1-0.2 s late: Helmholtz motion has to re-form", transform=ab.transAxes, fontsize=8, ha="right", color=CT)

    # (c) gate bars
    sc = Sub(3, 5, subplot_spec=outer[1, 0], hspace=0.75, wspace=0.55)
    keys = ["t_per", "att10_90", "att_tilt", "mod4_7", "mod10", "lev_sd", "vib_rate", "vib_depth", "vib_reg_m", "vib_onset", "hnr", "rel50", "rel100", "rel200"]
    short = {"t_per": "time to periodic s", "att10_90": "attack 10-90 ms", "att_tilt": "attack - sustain\n2-6 kHz dB", "mod4_7": "4-7 Hz mod. share", "mod10": "above-10 Hz share",
             "lev_sd": "level sd dB", "vib_rate": "vibrato rate Hz", "vib_depth": "vibrato depth c", "vib_reg_m": "vibrato regularity", "vib_onset": "vibrato onset s",
             "hnr": "HNR dB", "rel50": "release +50 ms dB", "rel100": "release +100 ms dB", "rel200": "release +200 ms dB"}
    first = None
    for i, k in enumerate(keys):
        ax = fig.add_subplot(sc[i // 5, i % 5])
        vals = [me[k], mn[k], mr[k]]
        vals = [0.0 if not np.isfinite(v) else v for v in vals]
        b = ax.bar([0, 1, 2], vals, color=[CE, CN, CT], width=0.8)
        for x, v in enumerate(vals):
            ax.text(x, v, ("%.3g" % v), ha="center", va="bottom" if v >= 0 else "top", fontsize=6.5)
        ax.set_title(short[k], fontsize=7.5)
        ax.set_xticks([])
        ax.tick_params(labelsize=6)
        ax.margins(y=0.25)
        ax.axhline(0, color="#888888", lw=0.5)
        if first is None: first = b
    lg = fig.add_subplot(sc[2, 4])
    lg.axis("off")
    for x, (c, l) in enumerate(((CE, "EXPERT"), (CN, "NOVICE (vibrato 6.5 Hz, 20 c)"), (CT, "TARGET Philharmonia"))):
        lg.add_patch(plt.Rectangle((0, 0.7 - 0.3 * x), 0.12, 0.2, color=c))
        lg.text(0.16, 0.74 - 0.3 * x, l, fontsize=7.5, va="center")
    lg.set_xlim(0, 1.2); lg.set_ylim(0, 1)
    fig.text(0.055, 0.49, "(c) gates: median of G2 D3 A3 (dry)", fontsize=10.5, ha="left")

    # (d) spectrograms of D3
    sd = Sub(1, 3, subplot_spec=outer[1, 1], wspace=0.08)
    ref = M.load_audio(G.PHIL_FULL % 50)
    segs = []
    for y, on in ((ref, G.onset_s(ref)), (ye, G.onset_s(ye[int(3.3 * SR):int(6.0 * SR)]) + 3.3), (yn, G.onset_s(yn[int(3.3 * SR):int(6.0 * SR)]) + 3.3)):
        s = y[max(int((on - 0.1) * SR), 0):int((on + 2.0) * SR)]
        s = s / (np.sqrt(np.mean(s[int(0.6 * SR):int(1.4 * SR)] ** 2)) + 1e-12)
        segs.append(s)
    for i, (s, lab, c) in enumerate(zip(segs, ["TARGET Philharmonia D3", "EXPERT D3", "NOVICE D3"], [CT, CE, CN])):
        ax = fig.add_subplot(sd[i])
        f, t, S = ss.spectrogram(s, SR, nperseg=4096, noverlap=3584, window="hann")
        db = 10 * np.log10(S + 1e-14)
        im = ax.pcolormesh(t - 0.1, f, db, vmin=-100, vmax=-20, cmap="magma", shading="auto")
        ax.set_ylim(0, 6000)
        ax.set_title(lab, fontsize=9, color=c, fontweight="bold")
        ax.set_xlabel("s", fontsize=8)
        ax.tick_params(labelsize=7)
        if i == 0: ax.set_ylabel("Hz", fontsize=8)
        else: ax.set_yticklabels([])
    cb = fig.colorbar(im, ax=fig.axes[-3:], fraction=0.02, pad=0.01)
    cb.set_label("dB (all three on one scale, notes at equal sustain rms)", fontsize=7)
    cb.ax.tick_params(labelsize=7)
    fig.text(0.54, 0.49, "(d) spectrograms of D3, same colour scale", fontsize=10.5, ha="left")
    txt = ("EXPERT median: time to periodic %.2f s, attack %.0f ms, 4-7 Hz share %.2f, level sd %.1f dB, vibrato %.1f Hz / %.1f c, HNR %.1f dB.   "
           "Philharmonia: %.2f s, %.0f ms, %.2f, %.1f dB, %.1f Hz / %.1f c, %.1f dB." % (me["t_per"], me["att10_90"], me["mod4_7"], me["lev_sd"], me["vib_rate"], me["vib_depth"], me["hnr"],
                                                                                         mr["t_per"], mr["att10_90"], mr["mod4_7"], mr["lev_sd"], mr["vib_rate"], mr["vib_depth"], mr["hnr"]))
    fig.text(0.5, 0.008, txt, fontsize=8.5, ha="center")
    fig.savefig(OUTPNG)
    print("wrote", OUTPNG)


if __name__ == "__main__":
    main()
