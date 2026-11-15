"""wa03_timing.py: closed-loop onset-time calibration of a notes TSV against the original.

Inputs: NOTES_IN.tsv, ORIG.wav, REPLICA.wav (a render of NOTES_IN.tsv by wa03_build.sh; render at -16 LUFS is fine, only times are read).
For each piano and cello note the envelope of its fundamental and 2nd and 3rd partials (STFT 2048 / hop 64 at 22050 Hz, linear power) is cut from
[start - 0.06, start + 0.15] in both signals; the time at which each reaches 50 percent of its own maximum over that window is the rise time.
The note moves by (rise in the original) - (rise in the replica), limited to +-35 ms and applied with --gain (default 0.8); the replica
render is first aligned to the original by the constant offset the cross-correlation of the two onset envelopes gives (compare.py reports it as offset_ms;
here read off the median difference, which is removed before the per-note moves).  Notes whose window has no clear rise (max / min under 3) stay.
usage: wa03_timing.py NOTES_IN.tsv ORIG.wav REPLICA.wav NOTES_OUT.tsv [--gain 0.8]     Run with the venv python (librosa).
"""
import argparse
import numpy as np, librosa

SR, N, H = 22050, 2048, 64


def env(S, fb, m):
    f0 = librosa.midi_to_hz(m); E = 0
    for h in (1, 2, 3):
        b = int(round(f0 * h / fb))
        if 1 < b < S.shape[0] - 2:
            E = E + S[b - 1:b + 2].max(0) ** 2
    return E


def rise(E, t):
    i0 = max(0, int((t - 0.06) * SR / H)); i1 = min(len(E), int((t + 0.15) * SR / H))
    w = E[i0:i1]
    if len(w) < 8 or w.max() / (w.min() + 1e-12) < 3:
        return None
    k = int(np.argmax(w > 0.5 * w.max()))
    return (i0 + k) * H / SR


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('inp'); ap.add_argument('orig'); ap.add_argument('rep'); ap.add_argument('out'); ap.add_argument('--gain', type=float, default=0.8)
    a = ap.parse_args()
    notes = []
    for ln in open(a.inp):
        if ln.strip() and not ln.startswith('#'):
            p, s, e, m, v = ln.split('\t')[:5]; notes.append((p, float(s), float(e), float(m), float(v)))
    yo = librosa.load(a.orig, sr=SR, mono=True)[0]; yr = librosa.load(a.rep, sr=SR, mono=True)[0]
    So = np.abs(librosa.stft(yo, n_fft=N, hop_length=H)) + 1e-7; Sr = np.abs(librosa.stft(yr, n_fft=N, hop_length=H)) + 1e-7
    fb = SR / N
    d = []
    for p, s, e, m, v in notes:
        if p == 'pedal':
            d.append(None); continue
        ro, rr = rise(env(So, fb, m), s), rise(env(Sr, fb, m), s)
        d.append(None if ro is None or rr is None else ro - rr)
    vals = np.array([x for x in d if x is not None])
    med = float(np.median(vals)) if len(vals) else 0.0
    out = []
    for (p, s, e, m, v), x in zip(notes, d):
        if x is not None:
            mv = float(np.clip(a.gain * (x - med), -0.035, 0.035)) + 0.0
            out.append((p, s + mv, e + mv, m, v))
        else:
            out.append((p, s, e, m, v))
    print('timing: %d of %d notes measured, median rise difference %.1f ms, spread (MAD) %.1f ms' % (len(vals), len(notes), med * 1000, 1000 * float(np.median(np.abs(vals - med))) if len(vals) else 0))
    with open(a.out, 'w') as f:
        for p, s, e, m, v in sorted(out, key=lambda n: (n[1], n[3])):
            f.write('%s\t%.4f\t%.4f\t%.2f\t%.5f\n' % (p, s, e, m, v))


if __name__ == '__main__':
    main()
