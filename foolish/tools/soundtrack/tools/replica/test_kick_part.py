# test_kick_part.py - test of kick_part.py: a synthetic kit (two kicks, a snare, a hat, a clap) and its events, plus the mutation check.
#
# What it does: writes five one-shot wavs and a replica directory (parts.txt, a manifest, score.txt with 5 EVENTS lines of
#   one part), runs kick_part.main, and asserts: the two kick samples are classed as kick and the other three not, KICK plus
#   DRUMS hold exactly the input event tokens (same strings, same count, no event in both parts), both files carry the
#   original gain. `python3 -I test_kick_part.py` then runs the mutation (a kick left in both parts), which must FAIL.
# Randomness: the noise samples come from numpy's default_rng with the one seed SEED.
import os, re, subprocess, sys, tempfile, importlib.util
import numpy as np
from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
SEED = 7
SR = 44100


def make(d):
    rng = np.random.default_rng(SEED)
    t = np.arange(int(0.4 * SR)) / SR
    k1 = np.sin(2 * np.pi * (45 + 80 * np.exp(-t / 0.02)) * t) * np.exp(-t / 0.12)
    k2 = np.sin(2 * np.pi * (55 + 120 * np.exp(-t / 0.03)) * t) * np.exp(-t / 0.2) + 0.05 * rng.standard_normal(len(t)) * np.exp(-t / 0.01)
    sn = rng.standard_normal(len(t)) * np.exp(-t / 0.08) + 0.3 * np.sin(2 * np.pi * 180 * t) * np.exp(-t / 0.05)
    hat = np.diff(rng.standard_normal(len(t) + 1)) * np.exp(-t / 0.03)
    cl = rng.standard_normal(len(t)) * np.exp(-t / 0.1) * (np.sin(2 * np.pi * 14 * t) > -0.5)
    man = []
    for i, w in enumerate([k1, sn, k2, hat, cl]):
        p = '%s/s%d.wav' % (d, i)
        wavfile.write(p, SR, (w * 0.5).astype(np.float32)); man.append('%d 0 0 %s' % (24 + i, p))
    open(d + '/man.txt', 'w').write('\n'.join(man) + '\n')
    open(d + '/parts.txt', 'w').write('PART DRUMS sampler man.txt gain_db 12.5 release_s 0.012 group drums\n')
    ev = ['0.0000,24,0.5000,0.8000', '0.5000,25,0.4000,0.7000', '1.0003,26,0.4000,0.9000', '1.5000,27,0.1000,0.3000', '2.0000,28,0.3000,0.5000', '2.5010,24,0.5000,0.6000', '3.0000,25,0.4000,0.7500']
    open(d + '/score.txt', 'w').write('TEMPO 120\nSECTION W BARS 2 KEY C\n# DRUMS gain_db 12.5000\nEVENTS DRUMS: ' + ' '.join(ev[:4]) + '\nEVENTS DRUMS: ' + ' '.join(ev[4:]) + '\n')
    return ev


def run(modpath):
    spec = importlib.util.spec_from_file_location('kick_part_under_test', modpath)
    K = importlib.util.module_from_spec(spec); sys.modules['kick_part_under_test'] = K
    spec.loader.exec_module(K)
    fails = []
    def need(c, msg):
        if not c:
            fails.append(msg)
        print(('ok   ' if c else 'FAIL ') + msg, flush=True)
    with tempfile.TemporaryDirectory() as d:
        ev = make(d)
        kick, rest = K.main(['--dir', d, '--out-prefix', d + '/o', '--samples-root', d])
        sh = {int(l.split('\t')[0]): l.split('\t')[3].strip() for l in open(d + '/o_shares.tsv').read().splitlines()[1:]}
        need(sh == {24: 'kick', 25: 'other', 26: 'kick', 27: 'other', 28: 'other'}, 'classes %s' % sh)
        read = lambda p: [t for l in open(p) if l.startswith('EVENTS') for t in l.split(':', 1)[1].split()]
        kt, dt = read(d + '/o_kick.txt'), read(d + '/o_drums.txt')
        need(sorted(kt + dt) == sorted(ev) and len(kt) + len(dt) == len(ev), 'KICK plus DRUMS are exactly the input events (%d + %d = %d)' % (len(kt), len(dt), len(ev)))
        need(not (set(kt) & set(dt)), 'no event is in both parts')
        need(all(open(d + '/o_%s.txt' % n).readline().strip() == '# %s gain_db 12.5000' % n.upper() for n in ('kick', 'drums')), 'both files carry the original gain')
    return fails


def main():
    if '--module' in sys.argv:
        sys.exit(1 if run(sys.argv[sys.argv.index('--module') + 1]) else 0)
    if run(os.path.join(HERE, 'kick_part.py')):
        print('FAIL test_kick_part'); sys.exit(1)
    src = open(os.path.join(HERE, 'kick_part.py')).read()
    new, n = re.subn(r'^KICK_ALSO_IN_DRUMS = .*$', 'KICK_ALSO_IN_DRUMS = True', src, flags=re.M)
    assert n == 1
    with tempfile.TemporaryDirectory() as d:
        open(d + '/kick_part.py', 'w').write(new)
        r = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--module', d + '/kick_part.py'], capture_output=True, text=True)
    first = [l for l in r.stdout.splitlines() if l.startswith('FAIL')]
    print('mutation a kick left in both parts -> %s %s' % ('killed' if r.returncode == 1 and first else 'SURVIVED/BROKEN', first[0][:110] if first else r.stderr[-200:]))
    ok = r.returncode == 1 and first
    print('PASS test_kick_part' if ok else 'FAIL test_kick_part')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
