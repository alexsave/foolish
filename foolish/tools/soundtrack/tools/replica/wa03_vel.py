"""wa03_vel.py: closed-loop velocity calibration of a notes TSV against the original.

Inputs: NOTES_IN.tsv (part, start_s, end_s, midi, velocity), the original wav, the replica wav rendered from NOTES_IN.tsv by wa03_build.sh.
For every piano note the energy of its fundamental and 2nd and 3rd partials in the first 0.25 s (CQT, hop 256 at 22050 Hz, 36 bins per octave)
is read from the original and from the replica; both sides are first scaled to the same total over all notes (the render is normalised
to -16 LUFS, the original is not).  The velocity moves by --gain per dB of difference (clipped to --step per pass); a cello note
compares its whole length and moves as (v + 0.05) * sqrt(ratio) - 0.05.  Run it two or three times, re-rendering in between.
usage: wa03_vel.py NOTES_IN.tsv ORIG.wav REPLICA.wav NOTES_OUT.tsv [--gain 0.02] [--step 0.15]
Run with the venv python (librosa).
"""
import argparse
import numpy as np, librosa

SR, HOP, BPO = 22050, 256, 36
FMIN = librosa.note_to_hz('C1')
NB = BPO * 7


def energies(notes, y):
    C = np.abs(librosa.cqt(y, sr=SR, hop_length=HOP, fmin=FMIN, n_bins=NB, bins_per_octave=BPO)) ** 2
    out = []
    for p, s, e, m, v in notes:
        i0 = int(s * SR / HOP); i1 = int((min(e, s + 0.25) if p == 'piano' else e) * SR / HOP) + 1
        i1 = max(i1, i0 + 2)
        tot = 0.0
        for h in (1, 2, 3):
            mm = m + 12 * np.log2(h)
            b = int(round((mm - 24) * 3))
            if 1 <= b < NB - 1:
                tot += C[b - 1:b + 2, i0:i1].sum() / (i1 - i0)
        out.append(tot + 1e-12)
    return np.array(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('inp'); ap.add_argument('orig'); ap.add_argument('rep'); ap.add_argument('out')
    ap.add_argument('--gain', type=float, default=0.02); ap.add_argument('--step', type=float, default=0.15)
    a = ap.parse_args()
    notes = []
    for ln in open(a.inp):
        if ln.strip() and not ln.startswith('#'):
            p, s, e, m, v = ln.split('\t')[:5]; notes.append((p, float(s), float(e), float(m), float(v)))
    sel = [n for n in notes if n[0] in ('piano', 'cello')]
    yo = librosa.load(a.orig, sr=SR, mono=True)[0]; yr = librosa.load(a.rep, sr=SR, mono=True)[0]
    Eo, Er = energies(sel, yo), energies(sel, yr)
    Er = Er * (Eo.sum() / Er.sum())
    db = 10 * np.log10(Eo / Er)
    new = []
    for (p, s, e, m, v), d in zip(sel, db):
        if p == 'piano':
            v2 = v + float(np.clip(a.gain * d, -a.step, a.step))
        else:
            v2 = (v + 0.05) * 10 ** (np.clip(d, -6, 6) / 20 / 2) - 0.05
        new.append((p, s, e, m, float(np.clip(v2, 0.04, 0.98))))
    new += [n for n in notes if n[0] == 'pedal']
    with open(a.out, 'w') as f:
        for p, s, e, m, v in sorted(new, key=lambda n: (n[1], n[3])):
            f.write('%s\t%.4f\t%.4f\t%.2f\t%.5f\n' % (p, s, e, m, v))
    print('velocity calibration: median |dB| %.2f, 90th %.2f' % (np.median(np.abs(db)), np.percentile(np.abs(db), 90)))


if __name__ == '__main__':
    main()
