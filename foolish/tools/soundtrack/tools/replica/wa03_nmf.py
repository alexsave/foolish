"""wa03_nmf.py: note-level transcription of the wa03 clip by fitting instrument templates to the CQT (analysis by synthesis).

The clip is one mix (Demucs puts piano and cello into one stem), so the split between the two is made here, by what fits.
Model of the magnitude CQT V (hop 512 at 22050 Hz, 36 bins per octave from C1):
  V ~ sum over piano candidates c of g_c * P_pitch(c) shifted to onset frame(c)     (P: Salamander note, decays, 4 s long)
    + sum over cello pitches p and frames f of A[p, f] * Cs_p                          (Cs: flat harmonic profile of the cello set, sustained)
Candidate piano onsets are where the basic-pitch onset posterior (permissive, bp.py output) is above a floor, dilated by one frame;
cello frames are where the basic-pitch note posterior at that pitch is above a floor.  Both gains are fitted with KL multiplicative
updates and an L1 penalty (the cello's is larger, so a note is cello only where a flat sustained stack explains it clearly better
than a decaying piano note).  Output: notes TSV (part, start, end, midi, velocity) for wa03_events.py.
usage: wa03_nmf.py --bp-onset bp_onset.npy --bp-note bp_note.npy --wav orig.wav --out notes.tsv [--lam-p 0.02 --lam-c 0.08 --iters 150 ...]
Run with the venv python (librosa).
"""
import argparse, re, os
import numpy as np, librosa, soundfile as sf

SR = 22050
HOP = 512
BPO = 36
FMIN = librosa.note_to_hz('C1')
NB = BPO * 7
FR = SR / HOP
MAN = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'samples', 'manifests')
SAMPLES = os.environ.get('SAMPLES', '/Users/alex/Dev/soundtrack-samples')


def read_manifest(name):
    out = []
    for ln in open(os.path.join(MAN, name)):
        m = re.match(r'(\d+) (\d+) (\d+) (.*)', ln.strip())
        if not m:
            continue
        path = m.group(4)
        i = path.find('/soundtrack/samples/')
        if i >= 0:
            path = SAMPLES + '/' + path[i + 20:]
        out.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), path))
    return out


def cqt_of(path, seconds):
    y, sr = sf.read(path, always_2d=True)
    y = y.mean(1)
    y = librosa.resample(y[:int(seconds * sr)], orig_sr=sr, target_sr=SR)
    return np.abs(librosa.cqt(y, sr=SR, hop_length=HOP, fmin=FMIN, n_bins=NB, bins_per_octave=BPO))


def shift_bins(M, k):
    out = np.zeros_like(M)
    if k >= 0:
        out[k:] = M[:NB - k] if k else M
    else:
        out[:NB + k] = M[-k:]
    return out


def piano_templates(layer=9, frames=170):
    man = [m for m in read_manifest('salamander.txt') if m[1] == layer]
    roots = sorted(set(m[0] for m in man))
    cache = {}
    T = {}
    for p in range(33, 101):
        r = min(roots, key=lambda x: abs(x - p))
        if r not in cache:
            path = [m[3] for m in man if m[0] == r][0]
            C = cqt_of(path, frames * HOP / SR + 0.1)[:, :frames]
            if C.shape[1] < frames:
                C = np.pad(C, ((0, 0), (0, frames - C.shape[1])))
            cache[r] = C / (C[:, :4].max() + 1e-9)
        T[p] = shift_bins(cache[r], (p - r) * 3)
    return T


def cello_templates(man_name='lyria_cello_flat.txt'):
    man = read_manifest(man_name)
    prof = {}
    for r in sorted(set(m[0] for m in man)):
        path = [m[3] for m in man if m[0] == r][0]
        C = cqt_of(path, 6.0)
        n = C.shape[1]
        v = np.median(C[:, int(n * 0.3):int(n * 0.7)], axis=1)
        prof[r] = v / v.max()
    roots = sorted(prof)
    T = {}
    for p in range(36, 80):
        r = min(roots, key=lambda x: abs(x - p))
        v = prof[r][:, None]
        T[p] = shift_bins(v, (p - r) * 3)[:, 0]
    return T


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bp-onset'); ap.add_argument('--bp-note'); ap.add_argument('--wav'); ap.add_argument('--out')
    ap.add_argument('--lam-p', type=float, default=0.02); ap.add_argument('--lam-c', type=float, default=0.08)
    ap.add_argument('--iters', type=int, default=150)
    ap.add_argument('--onset-floor', type=float, default=0.08)
    ap.add_argument('--note-floor', type=float, default=0.10)
    ap.add_argument('--piano-keep', type=float, default=0.03, help='keep piano candidates with gain above this fraction of the largest gain')
    ap.add_argument('--cello-keep', type=float, default=0.10)
    ap.add_argument('--no-cello', action='store_true')
    ap.add_argument('--save-model', default='')
    a = ap.parse_args()
    y = librosa.load(a.wav, sr=SR, mono=True)[0]
    V = np.abs(librosa.cqt(y, sr=SR, hop_length=HOP, fmin=FMIN, n_bins=NB, bins_per_octave=BPO)) + 1e-6
    nF = V.shape[1]
    PT = piano_templates()
    CT = cello_templates()
    # basic-pitch grid: 22050/256 frames per second, 88 pitches from A0 (21)
    bo = np.load(a.bp_onset)[0] if np.load(a.bp_onset).ndim == 3 else np.load(a.bp_onset)
    bn = np.load(a.bp_note)[0] if np.load(a.bp_note).ndim == 3 else np.load(a.bp_note)
    bfr = SR / 256.0
    def bp_at(arr, p, t):
        i = int(round(t * bfr)); return arr[min(max(i, 0), arr.shape[0] - 1), p - 21]
    # piano candidates: (pitch, onset frame) on the 23 ms grid where bp onset posterior over that frame's span (and a frame either side) is above the floor
    cands = []
    for p in range(36, 97):
        col = bo[:, p - 21]
        for f in range(nF):
            t0, t1 = (f - 1) * HOP / SR, (f + 2) * HOP / SR
            i0, i1 = max(0, int(t0 * bfr)), min(len(col), int(t1 * bfr) + 1)
            if i1 > i0 and col[i0:i1].max() > a.onset_floor:
                cands.append((p, f))
    cands = np.array(cands)
    print('piano candidates', len(cands))
    G = np.full(len(cands), 0.5)
    Lp = PT[36].shape[1]
    # cello activations
    cp = list(range(36, 77))
    Tc = np.stack([CT[p] for p in cp], 1)       # NB x P
    mask = np.zeros((len(cp), nF))
    for j, p in enumerate(cp):
        for f in range(nF):
            t = f * HOP / SR
            if max(bp_at(bn, p, t), bp_at(bn, p, t + 0.02)) > a.note_floor:
                mask[j, f] = 1
    A = mask * 0.2
    if a.no_cello:
        A[:] = 0
    print('cello cells', int(mask.sum()))
    tp = [PT[p] for p, f in cands]
    for it in range(a.iters):
        M = np.zeros_like(V)
        for (p, f), g, T in zip(cands, G, tp):
            L = min(Lp, nF - f)
            M[:, f:f + L] += g * T[:, :L]
        M += Tc @ A
        M += 1e-6
        R = V / M
        num = np.zeros(len(cands)); den = np.zeros(len(cands))
        for k, ((p, f), T) in enumerate(zip(cands, tp)):
            L = min(Lp, nF - f)
            num[k] = (T[:, :L] * R[:, f:f + L]).sum(); den[k] = T[:, :L].sum()
        G *= num / (den + a.lam_p)
        if not a.no_cello:
            A *= mask * ((Tc.T @ R) / (Tc.sum(0)[:, None] + a.lam_c))
        if it % 25 == 0 or it == a.iters - 1:
            kl = (V * np.log(V / M) - V + M).sum() / V.sum()
            print('iter', it, 'KL/sum %.4f' % kl, 'piano alive', int((G > 0.03 * G.max()).sum()), 'cello cells', int((A > a.cello_keep * max(A.max(), 1e-9)).sum()))
    if a.save_model:
        np.savez(a.save_model, cands=cands, G=G, A=A, cp=np.array(cp))
    notes = []
    gmax = G.max()
    # piano
    for (p, f), g in zip(cands, G):
        if g < a.piano_keep * gmax:
            continue
        t = f * HOP / SR
        notes.append(['piano', t, 0.0, float(p), float(g)])
    # cello: runs of active frames per pitch
    amax = max(A.max(), 1e-9)
    for j, p in enumerate(cp):
        act = A[j] > a.cello_keep * amax
        f = 0
        while f < nF:
            if act[f]:
                g0 = f
                gap = 0
                while f < nF and (act[f] or gap < 3):
                    gap = 0 if act[f] else gap + 1
                    f += 1
                g1 = f - gap
                if (g1 - g0) * HOP / SR >= 0.4:
                    notes.append(['cello', g0 * HOP / SR, g1 * HOP / SR, float(p), float(A[j, g0:g1].mean())])
            else:
                f += 1
    # piano note ends: the fundamental line of the original falls 15 dB under its level at the onset, or the next onset at that pitch, at most 6 s
    on = {}
    for n in notes:
        if n[0] == 'piano':
            on.setdefault(int(n[3]), []).append(n)
    for p, lst in on.items():
        lst.sort(key=lambda n: n[1])
        b = int(round((p - 24) * 3))
        line = V[max(0, b - 1):b + 2].max(0)
        for i, n in enumerate(lst):
            f0 = int(n[1] * FR)
            lim = lst[i + 1][1] if i + 1 < len(lst) else nF / FR
            ref = line[f0:f0 + 3].max()
            e = f0 + 2
            while e < nF and e / FR < min(lim, n[1] + 6) and line[e] > ref * 0.18:
                e += 1
            n[2] = max(e / FR, n[1] + 0.12)
    gp = [n[4] for n in notes if n[0] == 'piano']
    cmax = max([n[4] for n in notes if n[0] == 'cello'] or [1])
    for n in notes:   # velocity from the fitted gain: 30 dB of range below the loudest note of its part
        top = gmax if n[0] == 'piano' else cmax
        d = float(np.clip((20 * np.log10(max(n[4], 1e-9) / top) + 30) / 30, 0, 1))
        n[4] = 0.15 + 0.8 * d if n[0] == 'piano' else 0.3 + 0.5 * d
    with open(a.out, 'w') as fo:
        for n in sorted(notes, key=lambda n: (n[1], n[3])):
            fo.write('%s\t%.4f\t%.4f\t%.2f\t%.5f\n' % tuple(n))
    print('piano notes', sum(1 for n in notes if n[0] == 'piano'), 'cello notes', sum(1 for n in notes if n[0] == 'cello'))


if __name__ == '__main__':
    main()
