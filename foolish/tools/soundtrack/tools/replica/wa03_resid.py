"""wa03_resid.py: add the notes the render is missing, found from the residual between the original and the replica CQT.

Inputs: NOTES_IN.tsv, ORIG.wav, REPLICA.wav (a render of NOTES_IN.tsv).  Both CQTs (hop 256 at 22050 Hz, 36 bins per octave) are scaled to the same
total energy.  Per MIDI pitch p the fundamental line is the max of its 3 bins.  A missing note at p is a run of at least --min-s seconds where the original line
is at least --db dB over the replica's, the 2nd partial line (p + 12) is also over by 3 dB, and the original line is not a partial of something lower
that is already sounding in the original (the original line at p - 12 is at least 6 dB under the one at p).  Each run becomes a piano note at the
run's start (the onset is then refined by wa03_post.py) with velocity 0.5 (wa03_vel.py calibrates it); runs of at least --cello-s seconds in MIDI 36 to 74 become cello notes.
usage: wa03_resid.py NOTES_IN.tsv ORIG.wav REPLICA.wav NOTES_OUT.tsv [--db 9] [--min-s 0.25] [--cello-s 1.5]
Run with the venv python (librosa).
"""
import argparse
import numpy as np, librosa

SR, HOP, BPO = 22050, 256, 36
FMIN = librosa.note_to_hz('C1'); NB = BPO * 7


def lines(y):
    C = np.abs(librosa.cqt(y, sr=SR, hop_length=HOP, fmin=FMIN, n_bins=NB, bins_per_octave=BPO)) ** 2
    L = {}
    for p in range(36, 100):
        b = int(round((p - 24) * 3)); L[p] = C[b - 1:b + 2].max(0)
    return L, C


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('inp'); ap.add_argument('orig'); ap.add_argument('rep'); ap.add_argument('out')
    ap.add_argument('--db', type=float, default=9); ap.add_argument('--min-s', type=float, default=0.25); ap.add_argument('--cello-s', type=float, default=1.5)
    a = ap.parse_args()
    notes = []
    for ln in open(a.inp):
        if ln.strip() and not ln.startswith('#'):
            p, s, e, m, v = ln.split('\t')[:5]; notes.append((p, float(s), float(e), float(m), float(v)))
    yo = librosa.load(a.orig, sr=SR, mono=True)[0]; yr = librosa.load(a.rep, sr=SR, mono=True)[0]
    n = min(len(yo), len(yr)); Lo, Co = lines(yo[:n]); Lr, Cr = lines(yr[:n])
    sc = Co.sum() / Cr.sum()
    fr = SR / HOP
    floor = 1e-5 * Co.max()
    add = []
    for p in range(38, 90):
        o, r = Lo[p], Lr[p] * sc
        d = 10 * np.log10((o + floor) / (r + floor))
        d2 = 10 * np.log10((Lo[p + 12] + floor) / (Lr[p + 12] * sc + floor)) if p + 12 in Lo else np.zeros_like(d)
        low = 10 * np.log10((o + floor) / (Lo[p - 12] + floor)) if p - 12 in Lo else np.full_like(d, 99)
        ok = (d >= a.db) & (d2 >= 3) & (low >= 6) & (o > 300 * floor)
        i = 0
        while i < len(ok):
            if ok[i]:
                j = i
                while j < len(ok) and (ok[j] or (j + 1 < len(ok) and ok[j + 1])):
                    j += 1
                if (j - i) / fr >= a.min_s:
                    dur = (j - i) / fr
                    part = 'cello' if (dur >= a.cello_s and p <= 74) else 'piano'
                    add.append((part, i / fr, j / fr, float(p), 0.5))
                i = j
            else:
                i += 1
    print('added', len(add), 'notes:', ' '.join('%s%.1f@%.2f' % (x[0][0], x[3], x[1]) for x in add[:40]))
    notes += add
    with open(a.out, 'w') as f:
        for p, s, e, m, v in sorted(notes, key=lambda n: (n[1], n[3])):
            f.write('%s\t%.4f\t%.4f\t%.2f\t%.5f\n' % (p, s, e, m, v))


if __name__ == '__main__':
    main()
