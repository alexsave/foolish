# kick_part.py - splits a drums one-shot replica into KICK and DRUMS parts by each sample's low-band share.
#
# What it does: reads a replica directory's parts.txt (the DRUMS part's sampler manifest), score.txt (its EVENTS DRUMS lines
#   and the "# DRUMS gain_db X" line) and loads each sample the manifest names. A sample's low-band share = the energy of its
#   first 80 ms between 30 and 120 Hz (an FFT of the first 80 ms, mono) divided by the energy of the same 80 ms over all
#   frequencies. The values are sorted, the threshold sits in the middle of the largest gap between neighbours, and a sample
#   above it is a kick. The events of a kick sample go to KICK, all others to DRUMS, token for token as they were written, so
#   the union is exactly the input events (every event in exactly one part).
# Inputs: --dir REPLICA_DIR --out-prefix P [--part DRUMS] [--samples-root DIR (default $SAMPLES or /Users/alex/Dev/soundtrack-samples)]
# Outputs: P_kick.txt and P_drums.txt (events files, each "# NAME gain_db X" with the original part's gain, then EVENTS lines),
#   P_shares.tsv (per sample: root, file, low-band share, class), and on stdout the values and the threshold.
# How to run: python3 -I kick_part.py --dir ../../replica/se11_drop_0-42_shared --out-prefix work/drums
# Known failure modes: a sample that mixes a kick with a loud snare or a sub-heavy tom lands on the wrong side if its share is
#   near the threshold; the largest gap is only a good threshold when the two classes are separated.
# Randomness: none.
import argparse, os, sys
import numpy as np
from scipy.io import wavfile

WIN_S = 0.080
BAND = (30.0, 120.0)
KICK_ALSO_IN_DRUMS = False     # the test mutates this: a kick is left in both parts


def read_wav(path):
    sr, x = wavfile.read(path)
    if x.dtype == np.int16:
        x = x / 32768.0
    x = x.astype(np.float64)
    return sr, (x.mean(axis=1) if x.ndim == 2 else x)


def low_share(x, sr):
    seg = x[:int(WIN_S * sr)]
    sp = np.abs(np.fft.rfft(seg)) ** 2
    f = np.fft.rfftfreq(len(seg), 1.0 / sr)
    tot = sp.sum()
    return float(sp[(f >= BAND[0]) & (f <= BAND[1])].sum() / tot) if tot > 0 else 0.0


def split_threshold(vals):
    v = np.sort(np.asarray(vals, dtype=float))
    if len(v) < 2:
        return float(v[0]) if len(v) else 0.0
    k = int(np.argmax(np.diff(v)))
    return float((v[k] + v[k + 1]) / 2)


def resolve(path, root):
    key = '/soundtrack/samples/'
    if key in path:
        return os.path.join(root, path.split(key, 1)[1])
    return path


def classify(manifest_lines, root):
    """manifest_lines: "root layer rr path" -> {root: (path, share)}"""
    out = {}
    for l in manifest_lines:
        p = l.split(None, 3)
        if len(p) < 4 or l.startswith('#'):
            continue
        sr, x = read_wav(resolve(p[3].strip(), root))
        out[int(p[0])] = (p[3].strip(), low_share(x, sr))
    return out


def split_events(lines, shares, thr, part):
    """lines of score.txt; returns (gain_line, kick_events, drum_events) as lists of event tokens."""
    gain = None; kick, rest = [], []
    for l in lines:
        s = l.strip()
        if s.startswith('# %s gain_db' % part):
            gain = s
        elif s.startswith('EVENTS %s:' % part):
            for tok in s.split(':', 1)[1].split():
                pitch = int(round(float(tok.split(',')[1])))
                (kick if shares[pitch][1] > thr else rest).append(tok)
    if KICK_ALSO_IN_DRUMS:
        rest = sorted(rest + kick, key=lambda t: float(t.split(',')[0]))
    return gain, kick, rest


def write_events(path, name, gain_line, toks):
    g = gain_line.split()[3] if gain_line else '0'
    with open(path, 'w') as f:
        f.write('# %s gain_db %s\n' % (name, g))
        for k in range(0, len(toks), 40):
            f.write('EVENTS %s: %s\n' % (name, ' '.join(toks[k:k + 40])))


def main(argv=None):
    ap = argparse.ArgumentParser(description='Split a drums replica into KICK and DRUMS parts.')
    ap.add_argument('--dir', required=True); ap.add_argument('--out-prefix', required=True)
    ap.add_argument('--part', default='DRUMS')
    ap.add_argument('--threshold', type=float, default=None, help='override the largest-gap threshold')
    ap.add_argument('--samples-root', default=os.environ.get('SAMPLES', '/Users/alex/Dev/soundtrack-samples'))
    a = ap.parse_args(argv)
    man = None
    for l in open(os.path.join(a.dir, 'parts.txt')):
        t = l.split()
        if len(t) > 3 and t[0] == 'PART' and t[1] == a.part:
            man = t[t.index('sampler') + 1]
    if man is None:
        sys.exit('kick_part: no PART %s with a sampler in parts.txt' % a.part)
    shares = classify(open(os.path.join(a.dir, man)).read().splitlines(), a.samples_root)
    thr = split_threshold([v[1] for v in shares.values()]) if a.threshold is None else a.threshold
    gain, kick, rest = split_events(open(os.path.join(a.dir, 'score.txt')).read().splitlines(), shares, thr, a.part)
    d = os.path.dirname(a.out_prefix)
    if d:
        os.makedirs(d, exist_ok=True)
    write_events(a.out_prefix + '_kick.txt', 'KICK', gain, kick)
    write_events(a.out_prefix + '_drums.txt', 'DRUMS', gain, rest)
    with open(a.out_prefix + '_shares.tsv', 'w') as f:
        f.write('root\tfile\tlow_band_share\tclass\n')
        for r in sorted(shares):
            f.write('%d\t%s\t%.4f\t%s\n' % (r, shares[r][0], shares[r][1], 'kick' if shares[r][1] > thr else 'other'))
    print('low-band shares (30-120 Hz energy of the first 80 ms over its total): ' + ' '.join('%d:%.4f' % (r, shares[r][1]) for r in sorted(shares, key=lambda r: shares[r][1])))
    print('threshold %.4f (middle of the largest gap); kick samples %d of %d; events: kick %d, drums %d' % (thr, sum(v[1] > thr for v in shares.values()), len(shares), len(kick), len(rest)))
    return kick, rest


if __name__ == '__main__':
    main()
