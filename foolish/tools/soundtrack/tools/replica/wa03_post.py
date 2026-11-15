"""wa03_post.py: refine a notes TSV against the original audio: exact piano onset times and a pedal part.

Input: notes TSV from wa03_nmf.py (part, start_s, end_s, midi, velocity), the original wav (mono, any rate).
1. Piano onsets sit on the template fit's 23 ms grid; each is moved to the strongest rise of the pitch's own partials (fundamental, 2nd and
   3rd harmonic, STFT 2048 / hop 64 at 22050 Hz, rise = log energy now minus 4 frames earlier) within +-45 ms, then shifted by --shift-ms
   (the measured constant between the rise and the time the comparison tool's onset detector reports).  Note ends move with their starts.
2. Pedal: the damper is down all clip long and lifts for --lift-s at each harmony change, found as peaks of the low-register (MIDI 36 to 60)
   chroma change with at least --min-gap-s between them (written as PEDAL notes: start, end = the time the damper is up).
usage: wa03_post.py NOTES_IN.tsv ORIG.wav NOTES_OUT.tsv [--shift-ms 0] [--no-pedal] [--no-refine] [--lift-s 0.12] [--min-gap-s 1.8] [--chroma-thr 96  (percentile of the low-register rise curve)]
Run with the venv python (librosa).
"""
import sys, argparse
import numpy as np, librosa

SR = 22050


def refine(notes, y, shift):
    N, H = 2048, 64
    S = np.abs(librosa.stft(y, n_fft=N, hop_length=H, window='hann')) + 1e-7
    fb = SR / N
    out = []
    for p, s, e, m, v in notes:
        if p != 'piano':
            out.append((p, s, e, m, v)); continue
        f0 = librosa.midi_to_hz(m)
        bins = [int(round(f0 * h / fb)) for h in (1, 2, 3) if f0 * h < SR / 2 - 200]
        E = sum(S[b - 1:b + 2].max(0) ** 2 for b in bins if b > 1)
        L = np.log(E + 1e-9)
        d = L[4:] - L[:-4]
        i0 = int(max(0, (s - 0.045)) * SR / H); i1 = int((s + 0.045) * SR / H)
        i0 = max(i0, 0); i1 = min(i1, len(d) - 1)
        if i1 <= i0:
            out.append((p, s, e, m, v)); continue
        k = i0 + int(np.argmax(d[i0:i1 + 1]))
        t = (k + 2) * H / SR + shift          # the rise is centred between frame k and k + 4
        out.append((p, t, e + (t - s), m, v))
    return out


def pedal_events(y, lift, gap, thr, fixed=None):
    """Lift times = the strongest rises of the low register (MIDI 33 to 56, log CQT, 6 frames of 11.6 ms), at least gap seconds apart;
    thr is the percentile of the rise curve a peak must exceed (a low-register chord change is the pedal change a pianist makes)."""
    C = np.abs(librosa.cqt(y, sr=SR, hop_length=256, fmin=librosa.midi_to_hz(33), n_bins=24, bins_per_octave=12))
    L = np.log(C + 1e-4)
    d = np.maximum(0, L[:, 6:] - L[:, :-6]).sum(0)
    from scipy.signal import find_peaks
    p, _ = find_peaks(d, height=np.percentile(d, thr), distance=int(0.5 * SR / 256))
    cand = sorted(((d[i], (i + 3) * 256 / SR) for i in p), reverse=True)
    times = []
    for h, t in cand:
        if all(abs(t - u) >= gap for u in times):
            times.append(t)
    if fixed:
        times = sorted(fixed)
    times.sort()
    dur = len(y) / SR
    ev, start = [], 0.0
    for t in times + [dur + 1.0]:
        ev.append(('pedal', start, max(start + 0.1, t - 0.02), 60.0, 1.0))
        start = t - 0.02 + lift
    return ev, times


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('inp'); ap.add_argument('wav'); ap.add_argument('out')
    ap.add_argument('--shift-ms', type=float, default=0.0); ap.add_argument('--no-pedal', action='store_true'); ap.add_argument('--no-refine', action='store_true')
    ap.add_argument('--lift-s', type=float, default=0.12); ap.add_argument('--min-gap-s', type=float, default=1.8); ap.add_argument('--pedal-times', default='', help='comma list of lift times (s) read off the low-register rise curve, instead of the automatic pick'); ap.add_argument('--chroma-thr', type=float, default=96.0)
    a = ap.parse_args()
    y = librosa.load(a.wav, sr=SR, mono=True)[0]
    notes = []
    for ln in open(a.inp):
        if ln.strip() and not ln.startswith('#'):
            p, s, e, m, v = ln.split('\t')[:5]
            if p != 'pedal':
                notes.append((p, float(s), float(e), float(m), float(v)))
    if not a.no_refine:
        notes = refine(notes, y, a.shift_ms / 1000)
    if not a.no_pedal:
        ev, times = pedal_events(y, a.lift_s, a.min_gap_s, a.chroma_thr, [float(x) for x in a.pedal_times.split(',')] if a.pedal_times else None)
        notes += ev
        print('pedal lifts at', ' '.join('%.2f' % t for t in times))
    with open(a.out, 'w') as f:
        for p, s, e, m, v in sorted(notes, key=lambda n: (n[1], n[3])):
            f.write('%s\t%.4f\t%.4f\t%.2f\t%.5f\n' % (p, s, e, m, v))


if __name__ == '__main__':
    main()
