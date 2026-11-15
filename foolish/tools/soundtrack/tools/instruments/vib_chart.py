#!/usr/bin/env python3
"""vib_chart.py WAV SECTIONS.txt OUTDIR ITER LABEL : one figure PER INSTRUMENT (cello, haegeum, trumpet) for the OFF half of the
on-then-off demo, saved as OUTDIR/iteration_NN_<instrument>.png. Title (large): INSTRUMENT, what is plotted (OFF half vs a flat
tone, with the ON half in light grey), the iteration number and the note. Panels, all with a legend and unit labels: pitch track
in cents (sub-cent, vibmod), level in dB, levels of harmonics 1 to 4 in dB, modulation spectrum with the 3 to 8 Hz band and the
20 to 35 Hz floor, spectrogram. The flat target is the dashed line at zero modulation. The pp depth and peak/floor ratio of every
series are written on the figure. Uses the Agg backend (no window); open the PNG with `open`.
Module use: draw(wav, sections, outdir, it, label) -> {instrument: png path}."""
import os, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "samples"))
import lyria_notes_extract as le
import vib_demo_measure as dm
import vibmod

PICK = 2
HC = ["tab:blue", "tab:orange", "tab:green", "tab:red"]


def collect(wav, sections):
    got = {}
    x, sr = le.decode(wav)
    mono = x.mean(axis=1)
    for inst, half, nm, m, (a, e) in dm.notes(wav, sections):
        got.setdefault((inst, half), []).append((nm, m, mono[int(a * sr):int(e * sr)], sr))
    return got


def draw(wav, sections, outdir, it, label=""):
    got = collect(wav, sections)
    paths = {}
    for inst in ("cello", "haegeum", "trumpet"):
        nm, m, seg, sr = got[(inst, "off")][PICK]
        _, mo, _, _ = got[(inst, "on")][PICK]
        fig, ax = plt.subplots(3, 2, figsize=(15, 11))
        fig.suptitle("%s - OFF half (vibrato switch off) versus a perfectly flat tone - iteration %02d - note %s\n%s" % (inst.upper(), it, nm, label),
                     fontsize=17, fontweight="bold")
        def ser(a, key, k=None):
            v = a[key] if k is None else a[key][k]
            return np.arange(len(v)) / vibmod.DEC, v - np.median(v)
        a = ax[0, 0]
        a.plot(*ser(mo, "pitch_c"), color="0.7", lw=1, label="ON half (engine vibrato)")
        a.plot(*ser(m, "pitch_c"), "k", lw=1.2, label="OFF half")
        a.set_ylim(-20, 20); a.set_ylabel("pitch deviation (cents)"); a.set_xlabel("time in note (s)")
        a = ax[0, 1]
        a.plot(*ser(mo, "level_db"), color="0.7", lw=1, label="ON half")
        a.plot(*ser(m, "level_db"), "k", lw=1.2, label="OFF half total level")
        a.set_ylim(-3, 3); a.set_ylabel("level deviation (dB)"); a.set_xlabel("time in note (s)")
        a = ax[1, 0]
        for k in range(4):
            a.plot(*ser(m, "harm_db", k), HC[k], lw=1, label="OFF half, harmonic %d" % (k + 1))
        a.set_ylim(-8, 8); a.set_ylabel("harmonic level deviation (dB)"); a.set_xlabel("time in note (s)")
        a = ax[1, 1]
        fr, X = vibmod.modspec(m["pitch_c"]); a.plot(fr, X, "k", lw=1.4, label="pitch (cents)")
        fr, X = vibmod.modspec(m["level_db"]); a.plot(fr, X, "tab:purple", lw=1.4, label="total level (dB)")
        for k in range(4):
            fr, X = vibmod.modspec(m["harm_db"][k]); a.plot(fr, X, HC[k], lw=0.8, label="harmonic %d (dB)" % (k + 1))
        a.axvspan(3, 8, color="gold", alpha=0.2, label="vibrato band 3 to 8 Hz"); a.axvspan(20, 35, color="g", alpha=0.1, label="noise floor band")
        a.set_xlim(0, 36); a.set_ylim(bottom=0); a.set_xlabel("modulation frequency (Hz)"); a.set_ylabel("modulation amplitude (cents or dB)")
        a = ax[2, 0]
        a.specgram(seg, NFFT=4096, Fs=sr, noverlap=3584, cmap="magma", vmin=-110, vmax=-30)
        a.set_ylim(0, 6000); a.set_ylabel("frequency (Hz)"); a.set_xlabel("time in note (s)"); a.set_title("spectrogram of the OFF note (a flat tone: straight horizontal lines)", fontsize=9)
        a = ax[2, 1]
        a.axis("off")
        txt = "KEY MEASURES, OFF half, this note\n(peak-to-peak depth; peak over floor ratio, audible if >= %.0f)\n\n" % vibmod.RATIO_AUDIBLE
        txt += "                3 to 8 Hz band         8 to 16 Hz band\n"
        txt += "pitch      %6.2f c  (x%5.1f)\n" % (m["pitch"][4], m["pitch"][3])
        txt += "level      %6.2f dB (x%5.1f)    %6.2f dB (x%5.1f)\n" % (m["level"][4], m["level"][3], 2 * m["level2"][0], m["level2"][1])
        for k, h in enumerate(m["harm"]):
            txt += "harmonic %d %6.2f dB (x%5.1f)    %6.2f dB (x%5.1f)\n" % (k + 1, h[4], h[3], 2 * m["harm2"][k][0], m["harm2"][k][1])
        txt += "\nperfectly flat tone: 0.00 c, 0.00 dB, ratio x1\n\nDashed red = flat target. Grey = same note, switch ON.\nTarget: no peak in the gold band above the green floor."
        a.text(0, 0.98, txt, va="top", fontsize=10, family="monospace")
        for r in range(2):
            ax[0, r].axhline(0, color="r", ls="--", lw=1, label="flat tone (target)")
        ax[1, 0].axhline(0, color="r", ls="--", lw=1, label="flat tone (target)")
        for aa in (ax[0, 0], ax[0, 1], ax[1, 0], ax[1, 1]):
            aa.legend(fontsize=8, loc="upper left" if aa is not ax[1, 1] else "upper right")
        fig.tight_layout(rect=(0, 0, 1, 0.92))
        p = os.path.join(outdir, "iteration_%02d_%s.png" % (it, inst))
        fig.savefig(p, dpi=75)
        plt.close(fig)
        paths[inst] = p
    return paths


if __name__ == "__main__":
    print(draw(sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5] if len(sys.argv) > 5 else ""))
