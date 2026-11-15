# test_poly_notes.py - test of poly_notes.py on synthetic rows and a synthetic stem, plus its mutation check.
#
# What it does: builds a stem of six chord-like bursts (off any grid, rising level) and transcription rows around them, runs
#   poly_notes.process and asserts: a row 30 ms off its stem onset moves to the detected onset, a row 80 ms off does not move,
#   two rows of the same pitch with the first overlapping the second end 10 ms before it, a chord splits into HOOK (the top note,
#   at or above 67) and CHORD (the rest), a chord whose top note is under 67 has no HOOK, velocities rise with the stem level,
#   and the 40 ms row is dropped. `python3 -I test_poly_notes.py` runs it and then the mutations (snap to a 16th grid, no
#   snapping, the overlap rule off, the split inverted), each of which must FAIL; `--module PATH` runs the test on a copy.
# Randomness: none (the bursts are harmonic tones).
import os, re, subprocess, sys, tempfile, importlib.util
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRATE = 44100
BPM = 150.0
# burst onset (s) and amplitude; off the 16th grid (0.1 s at 150 bpm) by 31 ms or more
BURSTS = [(0.531, 0.15), (1.337, 0.22), (2.153, 0.33), (2.961, 0.5), (3.771, 0.75), (4.583, 1.0)]


def build():
    x = np.zeros(int(5.6 * SRATE))
    for t0, a in BURSTS:
        n = int(0.3 * SRATE); t = np.arange(n) / SRATE
        y = sum(np.sin(2 * np.pi * f * t) for f in (220, 330, 495, 880, 1760, 3520)) * np.exp(-t / 0.15) * np.minimum(1, t / 0.003) * a * 0.1
        i = int(round(t0 * SRATE)); x[i:i + n] += y
    return x


def rows():
    r = lambda on, du, m, v=60: dict(stem='other', onset=on, dur=du, midi=m, tvel=v)
    return [r(BURSTS[0][0] + 0.030, 0.4, 60),            # 30 ms late: snaps
            r(BURSTS[1][0] + 0.080, 0.4, 62),            # 80 ms late: stays
            r(BURSTS[2][0], 0.9, 64), r(BURSTS[2][0] + 0.250, 0.4, 64),        # same pitch, the first overlaps the second
            r(BURSTS[3][0], 0.4, 55), r(BURSTS[3][0] + 0.004, 0.4, 64), r(BURSTS[3][0] + 0.002, 0.4, 72),   # a chord with a top note at 72
            r(BURSTS[4][0], 0.4, 60), r(BURSTS[4][0], 0.4, 65),                # a chord whose top note is under 67
            r(BURSTS[5][0], 0.04, 70)]                                          # 40 ms: dropped


def run(modpath):
    spec = importlib.util.spec_from_file_location('poly_notes_under_test', modpath)
    P = importlib.util.module_from_spec(spec); sys.modules['poly_notes_under_test'] = P
    sys.path.insert(0, HERE); spec.loader.exec_module(P)
    x = build()
    out, info = P.process(rows(), x, SRATE, 0.0, BPM, None, 67)
    det, _, _ = P.C.detect_onsets(P.C.resample(x, SRATE))
    fails = []
    def need(c, msg):
        if not c:
            fails.append(msg)
        print(('ok   ' if c else 'FAIL ') + msg, flush=True)
    need(len(det) >= 6, 'the detector finds the bursts (%d onsets)' % len(det))
    byk = lambda midi, near: [r for r in out if r['midi'] == midi and abs(r['t_tr'] - near) < 0.2]
    a = byk(60, BURSTS[0][0] + 0.03)
    dnear = det[np.argmin(np.abs(det - BURSTS[0][0]))]
    need(len(a) == 1 and abs(a[0]['onset'] - dnear) < 1e-6 and a[0]['snapped'], 'the row 30 ms off moves to the detected onset (%.4f vs %.4f)' % (a[0]['onset'] if a else -1, dnear))
    b = byk(62, BURSTS[1][0] + 0.08)
    need(len(b) == 1 and abs(b[0]['onset'] - (BURSTS[1][0] + 0.08)) < 1e-9 and not b[0]['snapped'], 'the row 80 ms off keeps its transcribed time (%.4f)' % (b[0]['onset'] if b else -1))
    c = sorted([r for r in out if r['midi'] == 64 and abs(r['t_tr'] - BURSTS[2][0]) < 0.4], key=lambda r: r['onset'])
    need(len(c) == 2 and abs((c[0]['onset'] + c[0]['dur']) - (c[1]['onset'] - 0.010)) < 1e-6, 'same pitch: the first note ends 10 ms before the next (end %.4f, next %.4f)' % (c[0]['onset'] + c[0]['dur'] if c else -1, c[1]['onset'] if len(c) > 1 else -1))
    ch = [r for r in out if abs(r['t_tr'] - BURSTS[3][0]) < 0.05]
    need(sorted((r['midi'], r['part']) for r in ch) == [(55, 'CHORD'), (64, 'CHORD'), (72, 'HOOK')], 'chord split: %s' % sorted((r['midi'], r['part']) for r in ch))
    lo = [r for r in out if abs(r['t_tr'] - BURSTS[4][0]) < 0.05]
    need(len(lo) == 2 and all(r['part'] == 'CHORD' for r in lo), 'a group whose top note is under 67 is all CHORD')
    need(not any(r['midi'] == 70 for r in out), 'the 40 ms row is dropped')
    vs = []
    for k in (0, 2, 3, 4):      # the groups sitting on a burst (the others have a transcribed time between bursts); amplitudes rise 0.15, 0.33, 0.5, 0.75
        g = [r for r in out if abs(r['t_tr'] - BURSTS[k][0]) < 0.05]
        vs.append(g[0]['vel'] if g else -1)
    need(all(p < q for p, q in zip(vs, vs[1:])) and vs[0] >= 0, 'velocities rise with the stem level: %s' % ' '.join('%.3f' % v for v in vs))
    return fails


MUTATIONS = [('snap to a 16th grid', r"^SNAP_MODE = .*$", "SNAP_MODE = 'grid16'"),
             ('no snapping', r"^SNAP_MODE = .*$", "SNAP_MODE = 'none'"),
             ('overlap rule off', r"^OVERLAP_RULE = .*$", "OVERLAP_RULE = False"),
             ('split inverted', r"^SPLIT_INVERTED = .*$", "SPLIT_INVERTED = True")]


def main():
    if '--module' in sys.argv:
        sys.exit(1 if run(sys.argv[sys.argv.index('--module') + 1]) else 0)
    if run(os.path.join(HERE, 'poly_notes.py')):
        print('FAIL test_poly_notes'); sys.exit(1)
    src = open(os.path.join(HERE, 'poly_notes.py')).read()
    bad = 0
    for name, pat, rep in MUTATIONS:
        new, n = re.subn(pat, rep, src, flags=re.M)
        assert n == 1, name
        with tempfile.TemporaryDirectory() as d:
            open(d + '/poly_notes.py', 'w').write(new)
            r = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--module', d + '/poly_notes.py'], capture_output=True, text=True)
        first = [l for l in r.stdout.splitlines() if l.startswith('FAIL')]
        print('mutation %-22s -> %s %s' % (name, 'killed' if r.returncode == 1 and first else 'SURVIVED/BROKEN', first[0][:110] if first else r.stderr[-200:]))
        bad += not (r.returncode == 1 and first)
    print('PASS test_poly_notes' if not bad else 'FAIL test_poly_notes: %d mutations survived' % bad)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
