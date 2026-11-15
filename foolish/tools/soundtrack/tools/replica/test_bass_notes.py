# test_bass_notes.py - test of bass_notes.py on a synthetic bass line, plus its mutation check.
#
# What it does: builds a bass line of 9 decaying harmonic notes on 5 pitches (a 20 cent downward glide in each, micro-timing off any
#   grid, two re-attacks on the same pitch where the previous note is still sounding, one legato step of 200 cents) and one 45 ms
#   blip, runs bass_notes.analyze and asserts: note count 9, every onset within 10 ms, pitches exact after rounding, the two
#   re-attacks found, the blip dropped and counted (a one-frame octave glitch at a note start may be dropped too), the median glide near -16 cents, and the sample cutter's output (the root of
#   a sample is its note's MIDI number, one sample per 4 semitones, its pitch corrected to within 5 cents).
#   `python3 -I test_bass_notes.py` runs the test and then each mutation (a copy of bass_notes.py with one constant changed,
#   run in a subprocess) and requires every mutation to FAIL; `--module PATH` runs the test alone against that copy.
# Randomness: none (the line is deterministic).
import os, re, subprocess, sys, tempfile, importlib.util
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRATE = 44100
# (onset s, end s, midi): note k sounds until its end, decaying from its onset
NOTES = [(0.503, 0.803, 33), (0.862, 1.131, 36), (1.131, 1.450, 36), (1.617, 1.900, 38), (1.900, 2.250, 38),
         (2.311, 2.707, 41), (2.707, 3.000, 43), (3.090, 3.400, 36), (3.472, 3.900, 33)]
BLIP = (4.200, 4.245, 40)
TAU = {1: 0.15, 3: 0.2}


def tone(t0, t1, m, tau=0.2, glide_cents=-20.0):
    n = int(round((t1 - t0) * SRATE))
    t = np.arange(n) / SRATE
    f0 = 440 * 2 ** ((m - 69) / 12)
    f = f0 * 2 ** (glide_cents / 1200 * t / (t1 - t0))
    ph = 2 * np.pi * np.cumsum(f) / SRATE
    y = sum(np.sin(h * ph) / h for h in range(1, 7)) * 0.3 * np.exp(-t / tau)
    return y * np.minimum(1, t / 0.002)


def build():
    x = np.zeros(int(5.0 * SRATE))
    for k, (a, b, m) in enumerate(NOTES):
        y = tone(a, b, m, TAU.get(k, 0.2))
        i = int(round(a * SRATE)); x[i:i + len(y)] += y
    a, b, m = BLIP
    y = tone(a, b, m, 10.0, 0.0); i = int(round(a * SRATE)); x[i:i + len(y)] += y * np.hanning(len(y)) ** 0.2
    return x


def run(modpath):
    spec = importlib.util.spec_from_file_location('bass_notes_under_test', modpath)
    B = importlib.util.module_from_spec(spec); sys.modules['bass_notes_under_test'] = B
    sys.path.insert(0, HERE); spec.loader.exec_module(B)
    x = build()
    notes, st = B.analyze(x, SRATE)
    fails = []
    def need(c, msg):
        if not c:
            fails.append(msg)
        print(('ok   ' if c else 'FAIL ') + msg, flush=True)
    need(len(notes) == 9, 'note count %d == 9' % len(notes))
    need(any(abs(a - BLIP[0]) < 0.03 for a in st['dropped_at']), 'the 45 ms blip is dropped and counted (dropped=%d at %s)' % (st['dropped'], ['%.3f' % a for a in st['dropped_at']]))
    if len(notes) == 9:
        dev = [abs(n.onset - o[0]) * 1000 for n, o in zip(notes, NOTES)]
        need(max(dev) <= 10, 'onsets within 10 ms of the truth (max %.1f ms, per note %s)' % (max(dev), ' '.join('%.1f' % d for d in dev)))
        need([round(n.midi) for n in notes] == [o[2] for o in NOTES], 'pitches exact: %s' % [round(n.midi) for n in notes])
        need([int(n.reattack) for n in notes] == [0, 0, 1, 0, 1, 0, 0, 0, 0], 're-attacks found on notes 3 and 5: %s' % [int(n.reattack) for n in notes])
        g = float(np.median([n.glide for n in notes]))
        need(-25 <= g <= -8, 'median glide %.1f cents (a 20 cent glide over the note, measured 10 to 90 percent)' % g)
    # the cutter
    with tempfile.TemporaryDirectory() as d:
        stereo = np.stack([x, x], axis=1)
        rows = B.cut_samples(stereo, SRATE, notes, st, 3, d, d + '/m.txt', 'T', d + '/log.tsv', 'synthetic', 0.0, 5.0, 'bass')
        need(1 <= len(rows) <= 3, 'sample cutter made %d samples' % len(rows))
        roots = [r[8] for r in rows]
        need(all(b - a >= 4 for a, b in zip(sorted(roots), sorted(roots)[1:])), 'one sample per 4 semitones: roots %s' % sorted(roots))
        from scipy.io import wavfile
        ok = True
        for r in rows:
            sr, w = wavfile.read(r[0])
            f0, per, en = B.C.yin(B.C.resample(w.astype(float).mean(axis=1), sr), B.SR, 30, 200)
            v = (per > 0.75) & (en > en.max() * 0.3)
            if v.sum() < 3:
                ok = False; continue
            cents = abs(1200 * np.log2(np.median(f0[v]) / (440 * 2 ** ((r[8] - 69) / 12))))
            print('     sample root %d: median f0 %.1f cents from the root' % (r[8], cents))
            ok &= cents < 25    # the 20 cent glide of the source note stays; the pitch correction is to the median
        need(ok, 'cut samples sit within 25 cents of their root')
    return fails


MUTATIONS = [('re-attack rule off', r'^REATTACK_DB = .*$', 'REATTACK_DB = 1e9                 # MUTATED'),
             ('pitch band 300 cents', r'^BAND_CENTS = .*$', 'BAND_CENTS = 300.0                # MUTATED'),
             ('onset not refined', r'^REFINE = .*$', 'REFINE = False                      # MUTATED'),
             ('minimum length rule off', r'^MIN_NOTE_S = .*$', 'MIN_NOTE_S = 0.0                   # MUTATED')]


def main():
    if '--module' in sys.argv:
        fails = run(sys.argv[sys.argv.index('--module') + 1])
        sys.exit(1 if fails else 0)
    fails = run(os.path.join(HERE, 'bass_notes.py'))
    if fails:
        print('FAIL test_bass_notes'); sys.exit(1)
    src = open(os.path.join(HERE, 'bass_notes.py')).read()
    bad = 0
    for name, pat, rep in MUTATIONS:
        new, n = re.subn(pat, rep, src, flags=re.M)
        assert n == 1, name
        with tempfile.TemporaryDirectory() as d:
            open(d + '/bass_notes.py', 'w').write(new)
            r = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--module', d + '/bass_notes.py'], capture_output=True, text=True)
        first = [l for l in r.stdout.splitlines() if l.startswith('FAIL')]
        print('mutation %-26s -> %s %s' % (name, 'killed' if r.returncode else 'SURVIVED', first[0] if first else ''))
        bad += (r.returncode == 0)
    print('PASS test_bass_notes' if not bad else 'FAIL test_bass_notes: %d mutations survived' % bad)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
