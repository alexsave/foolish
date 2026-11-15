#!/usr/bin/env python3
"""long_chart_v7.py OLD.wav NEW.wav OUTDIR ITER OLDLABEL NEWLABEL : one figure per instrument and held note of the long notes demo
(lyria_instruments_demo/score_long.txt, render_long.sh: per instrument a 8 s note, 2 s rest, a second 8 s note), OLD left and NEW right.
Rows: spectrogram (0 to 6 kHz) of the note; the echo profile (for every 100 ms window of the whitened residual, the best correlation
with the residual 0.1 to 1.0 s earlier, over the note: a looped noise pattern shows as a line near 1; the 0.5 line is the threshold of residual_check);
harmonic 1 to 4 levels (dB against each one's median) and the total level of the note. Title (large): instrument, note, versions.
Saved as OUTDIR/long_NN_<instrument>_<note>.png."""
import os, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "..", "samples"))
import lyria_notes_extract as le
import vibmod, vib_demo_measure as dm, check_v7 as cv

NOTES = {"cello": ["D3", "G3"], "haegeum": ["C4", "G4"], "trumpet": ["D4", "A#4"]}
HC = ["tab:blue", "tab:orange", "tab:green", "tab:red"]


def echo_profile(r, sr, win=0.1, hop=0.05, lo=0.1, hi=1.0):
    W, H = int(win * sr), int(hop * sr)
    lags = np.arange(int(lo * sr), int(hi * sr), 24)
    t, b = [], []
    for a in range(int(hi * sr), len(r) - W, H):
        seg = r[a:a + W]
        e = np.dot(seg, seg)
        best = 0.0
        for L in lags:
            p = r[a - L:a - L + W]
            best = max(best, np.dot(seg, p) / np.sqrt(e * np.dot(p, p) + 1e-30))
        t.append(a / sr); b.append(best)
    return np.array(t), np.array(b)


def draw(old, new, outdir, it, ol, nl):
    data = [(le.decode(w)[0].mean(axis=1), le.decode(w)[1]) for w in (old, new)]
    paths = []
    for j, inst in enumerate(NOTES):
        for q, nm in enumerate(NOTES[inst]):
            t0 = j * 20 + q * 10
            off = 0.2 if t0 > 0 else 0.0     # seconds of the lead in before the note start in the cut
            fig, ax = plt.subplots(3, 2, figsize=(16, 11))
            fig.suptitle("%s - note %s held 8 s, vibrato off - %s (left, loops) versus %s (right) - iteration %02d" % (inst.upper(), nm, ol, nl, it), fontsize=13, fontweight="bold")
            for c, (m, sr) in enumerate(data):
                lab = (ol, nl)[c]
                seg = m[int((t0 - off) * sr):int((t0 + 8.8) * sr)]
                ax[0, c].specgram(seg, NFFT=4096, Fs=sr, noverlap=3584, cmap="magma", vmin=-110, vmax=-30)
                ax[0, c].set_ylim(0, 6000); ax[0, c].set_xlabel("time in note (s)"); ax[0, c].set_ylabel("frequency (Hz)"); ax[0, c].set_title("%s spectrogram" % lab, fontsize=10)
                f0 = vibmod.refine_f0(seg[int(0.6 * sr):int(1.4 * sr)], sr, dm.hz(dm.midi(nm)))
                pad = int(0.4 * sr)
                h, r = cv.residual_bw(seg, sr, f0)
                r = cv.whiten(r[pad:-pad]) 
                tt, bb = echo_profile(r, sr)
                ax[1, c].plot(tt + 0.4 - off, bb, "k", lw=1.0, label="best correlation of the whitened residual with itself 0.1 to 1.0 s earlier (%s)" % lab)
                ax[1, c].axhline(0.5, color="tab:red", lw=0.8, ls="--", label="0.5 (residual_check threshold)")
                ax[1, c].set_ylim(0, 1.05); ax[1, c].set_xlabel("time in note (s)"); ax[1, c].set_ylabel("echo correlation"); ax[1, c].legend(loc="upper right", fontsize=8)
                a = vibmod.analyse(seg[int(0.5 * sr):int(8.2 * sr)], sr, dm.hz(dm.midi(nm)))
                tl = 0.5 - off + np.arange(len(a["level_db"])) / vibmod.DEC
                for k in range(4):
                    ax[2, c].plot(tl, a["harm_db"][k] - np.median(a["harm_db"][k]), HC[k], lw=0.7, label="harmonic %d" % (k + 1))
                ax[2, c].plot(tl, a["level_db"] - np.median(a["level_db"]), "k", lw=1.2, label="total (harmonics 1 to 8)")
                ax[2, c].set_ylim(-6, 6); ax[2, c].set_xlabel("time in note (s)"); ax[2, c].set_ylabel("level against median (dB)"); ax[2, c].legend(loc="upper right", fontsize=7, ncol=5)
            fig.tight_layout(rect=(0, 0, 1, 0.95))
            p = os.path.join(outdir, "long_%02d_%s_%s.png" % (it, inst, nm.replace("#", "s")))
            fig.savefig(p, dpi=70)
            plt.close(fig)
            paths.append(p)
    return paths


if __name__ == "__main__":
    for p in draw(sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5], sys.argv[6]):
        print(p)
