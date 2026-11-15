# part_chart.py - one titled figure that sets a replica of ONE part against its original: pitch tracks, onsets, level, spectrograms.
#
# What it does: reads two wavs (the original stem and the replica's rendering of the same part), cuts them to the same window,
#   scales the replica to the original's RMS, and draws (top to bottom) the f0 track (compare.py's yin), the onset times of both
#   (compare.py's detector, original above the line, replica below it), the 20 ms RMS level in dB, and the two spectrograms
#   (ORIGINAL then REPLICA, 0-8 kHz log-free, dB). The figure has a large title naming the piece and part, a legend that names
#   every line (ORIGINAL, REPLICA), axis labels with units, and the score text written on the figure.
# Inputs: --orig WAV --replica WAV --title TEXT [--scores TEXT] [--start S --end S] [--fmin HZ --fmax HZ] --out PNG
# Outputs: the PNG; stdout prints the onset counts and the share of original onsets with a replica onset within 30 ms.
# How to run: python3 -I part_chart.py --orig bass.wav --replica bass_r.wav --title "SEOUL 11 0:00-0:42 - BASS - original vs replica - iteration 1" --scores "overall 56.3" --fmin 30 --fmax 200 --out bass.png
# Known failure modes: the pitch panel is meaningless for a polyphonic part (yin finds one f0); the replica is not time-aligned
#   (compare.py aligns by a constant offset; here an offset shows as a shift between the two onset rows).
# Randomness: none.
import argparse, os, sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare as C


def load(path, start, end):
    x = C.decode(path)[0]
    sr = 44100
    a, b = int(start * sr), int(end * sr) if end else len(x)
    return x[a:b], sr


def level_db(x, sr, win=0.02):
    w = int(win * sr)
    c = np.concatenate([[0], np.cumsum(x ** 2)])
    idx = np.arange(0, len(x) - w, w // 2)
    return (idx + w / 2) / sr, 10 * np.log10((c[idx + w] - c[idx]) / w + 1e-12)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--orig', required=True); ap.add_argument('--replica', required=True)
    ap.add_argument('--title', required=True); ap.add_argument('--scores', default='')
    ap.add_argument('--start', type=float, default=0.0); ap.add_argument('--end', type=float, default=0.0)
    ap.add_argument('--fmin', type=float, default=30); ap.add_argument('--fmax', type=float, default=400)
    ap.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    xo, sr = load(a.orig, a.start, a.end); xr, _ = load(a.replica, a.start, a.end)
    n = min(len(xo), len(xr)); xo, xr = xo[:n], xr[:n]
    xr = xr * (np.sqrt(np.mean(xo ** 2)) / max(np.sqrt(np.mean(xr ** 2)), 1e-12))
    fig, ax = plt.subplots(5, 1, figsize=(16, 15), sharex=True)
    fig.suptitle(a.title, fontsize=22, fontweight='bold')
    t0 = a.start
    fo, po, eo = C.yin(C.resample(xo, sr), C.SR, a.fmin, a.fmax); fr, pr, er = C.yin(C.resample(xr, sr), C.SR, a.fmin, a.fmax)
    ts = (np.arange(len(fo)) * 512 / C.SR) + t0
    vo = (po > 0.75) & (eo > 10 ** (-45 / 20) * eo.max()); vr = (pr > 0.75) & (er > 10 ** (-45 / 20) * max(er.max(), 1e-12))
    ax[0].semilogy(ts[vo], fo[vo], '.', ms=4, color='black', label='ORIGINAL pitch')
    ax[0].semilogy(ts[vr], fr[vr], '.', ms=3, color='tab:red', label='REPLICA pitch')
    ax[0].set_ylabel('f0 (Hz, log)'); ax[0].legend(loc='upper right'); ax[0].set_title('Pitch track (yin)')
    to = C.detect_onsets(C.resample(xo, sr))[0] + t0; tr = C.detect_onsets(C.resample(xr, sr))[0] + t0
    ax[1].vlines(to, 0, 1, color='black', lw=1, label='ORIGINAL onsets (%d)' % len(to))
    ax[1].vlines(tr, -1, 0, color='tab:red', lw=1, label='REPLICA onsets (%d)' % len(tr))
    ia, ib = C.match(to - t0, tr - t0, 0.03)
    ax[1].set_yticks([]); ax[1].legend(loc='upper right'); ax[1].set_title('Onset times (up = original, down = replica); matched within 30 ms: %d' % len(ia))
    lo, db_o = level_db(xo, sr); lr, db_r = level_db(xr, sr)
    ax[2].plot(lo + t0, db_o, color='black', lw=1, label='ORIGINAL level'); ax[2].plot(lr + t0, db_r, color='tab:red', lw=1, label='REPLICA level')
    ax[2].set_ylim(db_o.max() - 60, db_o.max() + 3); ax[2].set_ylabel('RMS 20 ms (dB)'); ax[2].legend(loc='upper right'); ax[2].set_title('Level')
    for k, (x, nm) in enumerate(((xo, 'ORIGINAL'), (xr, 'REPLICA'))):
        S = np.abs(np.fft.rfft(np.lib.stride_tricks.sliding_window_view(x, 2048)[::512] * np.hanning(2048), axis=1))
        f = np.fft.rfftfreq(2048, 1 / sr); m = f <= 8000
        ax[3 + k].imshow(20 * np.log10(S[:, m].T + 1e-9), origin='lower', aspect='auto', cmap='magma', vmin=-20, vmax=60,
                         extent=[t0, t0 + len(x) / sr, 0, 8])
        ax[3 + k].set_ylabel('frequency (kHz)'); ax[3 + k].set_title('%s spectrogram' % nm)
    ax[4].set_xlabel('time in the clip (s)')
    fig.text(0.01, 0.005, a.scores, fontsize=14, family='monospace')
    fig.tight_layout(rect=(0, 0.03, 1, 0.97)); fig.savefig(a.out, dpi=70)
    print('onsets orig %d replica %d matched30 %.3f' % (len(to), len(tr), len(ia) / max(len(to), 1)))


if __name__ == '__main__':
    main()
