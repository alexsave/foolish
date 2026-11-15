#!/usr/bin/env python3
"""warsaw_compare_v7.py V6.wav V7.wav OUT.png TITLE : compares two renders of the Warsaw passage 1 (75.86 to 111.59 s, render_p1.sh): integrated
loudness (ffmpeg ebur128), long term level per band (dB), the level of the difference signal (v7 minus v6, the piano and the rest are the same note
for note, so the difference is the cello change) against the v6 level, and a figure: spectrogram of both (0 to 8 kHz) and the long term spectra.
Prints the numbers. Loop jumps are counted separately by count_jumps_v7.sh."""
import os, subprocess, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "..", "samples"))
import lyria_notes_extract as le


def lufs(p):
    r = subprocess.run(["ffmpeg", "-hide_banner", "-i", p, "-af", "ebur128", "-f", "null", "-"], capture_output=True, text=True).stderr
    return float([l for l in r.splitlines() if " I:" in l][-1].split()[1])


def main():
    a, b, out, title = sys.argv[1:5]
    xa, sr = le.decode(a); xb, _ = le.decode(b)
    n = min(len(xa), len(xb))
    xa, xb = xa[:n], xb[:n]
    print("loudness LUFS v6 %.2f v7 %.2f" % (lufs(a), lufs(b)))
    ma, mb = xa.mean(axis=1), xb.mean(axis=1)
    N = 8192; w = np.hanning(N); fq = np.fft.rfftfreq(N, 1 / sr)
    def psd(m):
        return np.mean([np.abs(np.fft.rfft(m[i:i + N] * w)) ** 2 for i in range(0, len(m) - N, N // 2)], 0)
    Pa, Pb = psd(ma), psd(mb)
    bands = [(30, 100), (100, 300), (300, 1000), (1000, 3000), (3000, 6000), (6000, 12000), (12000, 20000)]
    print("band Hz: v6 dB, v7 dB, v7 - v6")
    for lo, hi in bands:
        s = (fq >= lo) & (fq < hi)
        da, db = 10 * np.log10(Pa[s].sum()), 10 * np.log10(Pb[s].sum())
        print("  %5d-%5d  %6.1f %6.1f %+5.1f" % (lo, hi, da, db, db - da))
    d = mb - ma
    print("difference signal rms %.1f dB re v6 rms (mean over the passage)" % (20 * np.log10(np.sqrt(np.mean(d ** 2)) / np.sqrt(np.mean(ma ** 2)))))
    fig, ax = plt.subplots(2, 2, figsize=(16, 9))
    fig.suptitle(title, fontsize=13, fontweight="bold")
    for c, (m, lab) in enumerate(((ma, "v6"), (mb, "v7"))):
        ax[0, c].specgram(m, NFFT=4096, Fs=sr, noverlap=3584, cmap="magma", vmin=-120, vmax=-30)
        ax[0, c].set_ylim(0, 8000); ax[0, c].set_xlabel("time in passage (s)"); ax[0, c].set_ylabel("frequency (Hz)"); ax[0, c].set_title("%s spectrogram" % lab, fontsize=10)
    ax[1, 0].semilogx(fq[1:], 10 * np.log10(Pa[1:] + 1e-20), "tab:blue", lw=0.7, label="v6")
    ax[1, 0].semilogx(fq[1:], 10 * np.log10(Pb[1:] + 1e-20), "tab:orange", lw=0.7, label="v7")
    ax[1, 0].set_xlim(50, 20000); ax[1, 0].set_ylim(-20, 90); ax[1, 0].set_xlabel("frequency (Hz)"); ax[1, 0].set_ylabel("long term level (dB)"); ax[1, 0].legend()
    t = np.arange(0, n - 4410, 4410) / sr
    ea = [20 * np.log10(np.sqrt(np.mean(ma[i:i + 4410] ** 2)) + 1e-9) for i in range(0, n - 4410, 4410)]
    ed = [20 * np.log10(np.sqrt(np.mean(d[i:i + 4410] ** 2)) + 1e-9) for i in range(0, n - 4410, 4410)]
    ax[1, 1].plot(t, ea, "tab:blue", lw=0.7, label="v6 level (100 ms RMS)")
    ax[1, 1].plot(t, ed, "tab:red", lw=0.7, label="difference v7 - v6")
    ax[1, 1].set_xlabel("time in passage (s)"); ax[1, 1].set_ylabel("dBFS"); ax[1, 1].legend(loc="lower right")
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    fig.savefig(out, dpi=70)


if __name__ == "__main__":
    main()
