#!/usr/bin/env python3
"""artifact_chart.py OLD.wav NEW.wav OUTDIR ITER OLDLABEL NEWLABEL : one figure per instrument (CELLO, HAEGEUM, TRUMPET) comparing two
renders of the C major scale demo (see artifact_check.py), saved as OUTDIR/artifacts_NN_<instrument>.png. Rows: level envelope of the
whole 20 s section (5 ms RMS, dB) old above new; harmonic 1 to 4 level (dB, relative to each note's median) of every note laid
end to end, old and new; spectrogram of notes 1 to 4, old and new; the artifact_check numbers are written on the figure.
Title (large): instrument, what is compared, iteration."""
import os, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "samples"))
import lyria_notes_extract as le
import vibmod, artifact_check as ac, vib_demo_measure as dm

HC = ["tab:blue", "tab:orange", "tab:green", "tab:red"]


def harm_series(m, sr, j, up):
    seq = up + up[-2::-1]
    T, H = [], [[] for _ in range(4)]
    for i, nm in enumerate(seq):
        t0 = j * 20 + i * 1.3
        r = vibmod.measure_note(m[int((t0 + 0.10) * sr):int((t0 + 0.95) * sr)], sr, dm.hz(dm.midi(nm)))
        n = len(r["harm_db"][0])
        T.append(t0 + 0.10 + np.arange(n) / vibmod.DEC + 0.0)
        for k in range(4):
            H[k].append(r["harm_db"][k] - np.median(r["harm_db"][k]))
    return np.concatenate(T), [np.concatenate(h) for h in H]


def draw(old, new, outdir, it, ol, nl):
    data = []
    for w in (old, new):
        x, sr = le.decode(w)
        data.append((x.mean(axis=1), sr, ac.check(w)))
    paths = {}
    for j, (inst, up) in enumerate(ac.SCALE.items()):
        fig, ax = plt.subplots(4, 2, figsize=(16, 13))
        fig.suptitle("%s - level envelope, harmonic levels and spectrogram of the C major scale, %s (left) versus %s (right) - iteration %02d" % (inst.upper(), ol, nl, it), fontsize=11, fontweight="bold")
        for c, (m, sr, chk) in enumerate(data):
            lab = (ol, nl)[c]
            sec = m[int(j * 20 * sr):int((j * 20 + 20) * sr)]
            e = ac.env_db(sec, sr, 0.005)
            ax[0, c].plot(np.arange(len(e)) * 0.005, e, "k", lw=0.6, label="level, 5 ms RMS (%s)" % lab)
            ax[0, c].set_ylim(-70, 0); ax[0, c].set_xlabel("time in section (s)"); ax[0, c].set_ylabel("level (dBFS)"); ax[0, c].legend(loc="lower right", fontsize=8)
            t, H = harm_series(m, sr, j, up)
            for k in range(4):
                ax[1, c].plot(t - j * 20, H[k], HC[k], lw=0.6, label="harmonic %d" % (k + 1))
            ax[1, c].set_ylim(-12, 12); ax[1, c].set_xlabel("time in section (s)"); ax[1, c].set_ylabel("harmonic level vs note median (dB)"); ax[1, c].legend(loc="upper right", fontsize=7, ncol=4)
            ax[2, c].specgram(sec[:int(5.2 * sr)], NFFT=4096, Fs=sr, noverlap=3584, cmap="magma", vmin=-110, vmax=-30)
            ax[2, c].set_ylim(0, 5000); ax[2, c].set_xlabel("time in section (s), notes 1 to 4"); ax[2, c].set_ylabel("frequency (Hz)")
            r = chk[inst]
            ax[3, c].axis("off")
            ax[3, c].text(0, 0.95, "%s: %s\n\nrepeat-ripple (envelope pattern at 40-300 ms lag): median %.2f dB, worst %.2f dB\nworst single-harmonic dip inside a note: median %.1f dB, worst %.1f dB\nlevel 50 ms before onsets: median %.1f dBFS, worst %.1f dBFS\nlargest 10 ms level step: median %.1f dB, worst %.1f dB\n\nspectrogram: a clean note is straight horizontal lines;\nghost copies show as faint repeats before or after notes." % (
                inst.upper(), lab, np.median(r[:, 0]), r[:, 0].max(), np.median(r[:, 1]), r[:, 1].max(), np.median(r[:, 2]), r[:, 2].max(), np.median(r[:, 3]), r[:, 3].max()), va="top", fontsize=10, family="monospace")
        fig.tight_layout(rect=(0, 0, 1, 0.95))
        p = os.path.join(outdir, "artifacts_%02d_%s.png" % (it, inst))
        fig.savefig(p, dpi=70)
        plt.close(fig)
        paths[inst] = p
    return paths


if __name__ == "__main__":
    print(draw(sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5], sys.argv[6]))
