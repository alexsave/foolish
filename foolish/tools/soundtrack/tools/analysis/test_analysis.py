# test_analysis.py - tests of the analysis tools on synthetic signals (plain asserts, exit code 0 or 1).
#
# What it does: checks tempo and key on two synthetic loops (97 BPM A minor, 184 BPM G minor), the melody and
#   bass note names of the per-bar table on a synthetic tune with known notes, the vibrato term of the voice
#   heuristic, determinism of the one random source, and greps this folder for stray random sources.
# Inputs: none (all signals are generated here from lib.rng()). Outputs: PASS or FAIL lines, exit code 0 or 1.
# How to run: python3 -I test_analysis.py        (about 30 s)
# Known failure modes: the tempo test fails if lib.py's lag-to-BPM conversion or key profiles are changed; the
#   grep test also fails on a comment that contains a banned pattern (reword the comment).
import sys, os, re, glob, traceback
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np
import lib
from lib import run_core, bar_table, voice_heuristic, stft_mag, rng, SR
from lib import ndimage

def tri(r, minor): return [r, r+(3 if minor else 4), r+7]
A_ROOT = 57; G_ROOT = 55

def synth(bpm, chords, dur=40., sr=44100):
    """kick on every beat, quiet noise hat on the off-beats, harmonic chord tones, one chord per bar of 4 beats."""
    g = rng()
    n = int(dur*sr); y = np.zeros(n)
    beat = 60./bpm
    t = np.arange(n)/sr
    for k in range(int(dur/(4*beat))+1):
        t0 = k*4*beat; i0 = int(t0*sr); i1 = min(n, int((t0+4*beat)*sr))
        ch = chords[k % len(chords)]
        tt = t[i0:i1]-t0
        env = np.minimum(1, tt/0.02)*np.minimum(1, (4*beat-tt)/0.03)
        for m in ch:
            f = 440*2**((m-69)/12.)
            for h, a in ((1, 1), (2, .5), (3, .3)):
                y[i0:i1] += 0.08*a*np.sin(2*np.pi*f*h*tt)*env
    for b in range(int(dur/beat)+1):
        i0 = int(b*beat*sr)
        L = int(0.08*sr)
        if i0+L > n: break
        tt = np.arange(L)/sr
        acc = 1.0 if b % 4 == 0 else 0.7
        y[i0:i0+L] += acc*0.5*np.sin(2*np.pi*(55+120*np.exp(-tt*60))*tt)*np.exp(-tt*40)
        j0 = int((b+0.5)*beat*sr); Lh = int(0.02*sr)
        if j0+Lh < n: y[j0:j0+Lh] += 0.02*g.standard_normal(Lh)*np.exp(-np.arange(Lh)/(0.004*sr))
    return (y/np.abs(y).max()*0.7).astype(np.float32)

CASES = [('97 BPM A minor', 97, [tri(A_ROOT, 1), tri(A_ROOT+5, 1), [A_ROOT+7, A_ROOT+11, A_ROOT+14], tri(A_ROOT, 1)], 'A minor'),
         ('184 BPM G minor', 184, [tri(G_ROOT, 1), tri(G_ROOT+5, 1), [G_ROOT+7, G_ROOT+11, G_ROOT+14], tri(G_ROOT, 1)], 'G minor')]

def test_tempo_and_key():
    for name, bpm, chords, key in CASES:
        r = run_core(None, mono_override=synth(bpm, chords))
        assert abs(r['bpm']/bpm-1) < 0.01, '%s: measured %.2f BPM, true %d (more than 1%% off)' % (name, r['bpm'], bpm)
        assert r.get('key') == key, '%s: measured key %s, true %s' % (name, r.get('key'), key)

def melody_signal():
    """97 BPM loop plus a known melody (per half beat) and bass (per bar A1 D2 E2 A1)."""
    bpm = 97; beat = 60/bpm; sr = 44100
    y = synth(bpm, CASES[0][2])
    t = np.arange(len(y))/sr
    yy = np.zeros_like(y)
    nb = int(len(y)/sr/beat)
    for b in range(nb):
        i0 = int(b*beat*sr); i1 = int((b+1)*beat*sr)
        tt = t[i0:i1]-b*beat
        bm = [33, 38, 40, 33][(b//4) % 4]
        fb = 440*2**((bm-69)/12)
        for h, a in ((1, 1), (2, .6), (3, .3)): yy[i0:i1] += 0.15*a*np.sin(2*np.pi*fb*h*tt)*np.minimum(1, tt/0.01)
        for hh in (0, 1):
            j0 = int((b+hh*0.5)*beat*sr); j1 = int((b+hh*0.5+0.5)*beat*sr)
            tt2 = t[j0:j1]-(b+hh*0.5)*beat
            mm = [81, 84, 88, 84, 83, 86, 89, 86][((b % 4)*2+hh)] if (b//4) % 4 == 0 else [74, 77, 81, 77, 76, 80, 83, 80][((b % 4)*2+hh)]
            f = 440*2**((mm-69)/12)
            for h, a in ((1, 1), (2, .5), (3, .3), (4, .2)): yy[j0:j1] += 0.12*a*np.sin(2*np.pi*f*h*tt2)*np.minimum(1, tt2/0.01)*np.minimum(1, (0.5*beat-tt2)/0.01)
    return (y*0.6+yy).astype(np.float32)

MEL_TRUTH = {'A5', 'C6', 'E6', 'B5', 'D6', 'F6', 'D5', 'F5', 'E5', 'G#5'}
BASS_TRUTH = {'A1', 'D2', 'E2'}

def table_notes(rows):
    mel = []; bass = []
    for ln in rows[2:]:
        c = [x.strip() for x in ln.strip('|').split('|')]
        bass += [n for n in c[-3].split() if n != '-']   # columns counted from the right: a half-bar chord cell contains a pipe
        mel += [n.rstrip('?') for n in c[-2].split() if n != '-']
    return mel, bass

def test_melody_and_bass():
    r = run_core(None, mono_override=melody_signal())
    mel, bass = table_notes(bar_table(r, 2.0, 22.0, r['tuning']))
    assert len(mel) >= 30 and len(bass) >= 15, 'too few notes in the table: %d melody, %d bass' % (len(mel), len(bass))
    fm = sum(n in MEL_TRUTH for n in mel)/len(mel); fb = sum(n in BASS_TRUTH for n in bass)/len(bass)
    assert fm >= 0.8, 'only %.0f%% of melody notes are in the true note set (%s)' % (100*fm, ' '.join(mel[:16]))
    assert fb >= 0.8, 'only %.0f%% of bass notes are in the true set (%s)' % (100*fb, ' '.join(bass[:16]))

def test_vibrato():
    sr = 22050; t = np.arange(int(8*sr))/sr
    def tone(vib_cents, vib_hz):
        f = 440*2**((vib_cents*np.sin(2*np.pi*vib_hz*t))/1200)
        ph = 2*np.pi*np.cumsum(f)/sr
        y = sum((0.7**h)*np.sin(h*ph) for h in range(1, 9))
        return (0.3*y/np.abs(y).max()).astype(np.float32)
    def vib_term(y):
        S4 = stft_mag(y, 4096, 512, maxhz=6000)
        return max(x[5] for x in voice_heuristic({'S4': S4}, 0.))
    v = vib_term(tone(40, 6)); s = vib_term(tone(0, 6)); w = vib_term(tone(40, 1.5))
    assert v > 0.9, 'vibrato term of a 6 Hz +-40 cent tone is %.2f, expected above 0.9' % v
    assert s == 0.0, 'vibrato term of a steady tone is %.2f, expected 0' % s
    assert w == 0.0, 'vibrato term of a 1.5 Hz wobble is %.2f, expected 0' % w

def test_rng_deterministic():
    assert np.array_equal(rng().random(8), rng().random(8)), 'rng() with the default seed is not deterministic'
    assert not np.array_equal(rng(1).random(8), rng(2).random(8)), 'different seeds give the same draws'

# Patterns of random or time-based sources.  Built from pieces so that this file does not match itself.
IMP = 'im' + 'port'
BANNED = [r'np\.' + 'random', r'numpy\.' + 'random', r'\b' + IMP + r'\b[^\n]*\b' + 'random' + r'\b', r'\bfrom\s+' + 'random' + r'\b', r'\b' + IMP + r'\b[^\n]*\b' + 'time' + r'\b', r'\bfrom\s+' + 'time' + r'\b', '__' + IMP + '__', IMP + 'lib', r'\b' + 'random' + r'\.',
          r'default_' + 'rng', 'Random' + 'State', r'\b' + 'rand' + r'\(', r'\b' + 'randn' + r'\(', 'arc4' + 'random', 'Math\\.' + 'random',
          r'time\.' + 'time', r'time\.' + 'time_ns', 'perf_' + 'counter', 'date' + r'time\.' + 'now', 'Date' + r'\.' + 'now', r'\b' + 'srand' + r'\(', r'os\.' + 'urandom', r'\b' + 'sec' + 'rets' + r'\b']
ALLOW = 'RNG-SOURCE'   # lib.py marks its single generator line with this comment; exactly one line may carry it

def stray_random_sources(folder):
    hits = []; marked = 0
    for p in sorted(glob.glob(os.path.join(folder, '*.py'))):
        with open(p) as f: lines = f.read().split('\n')
        for i, ln in enumerate(lines):
            if ALLOW in ln and os.path.basename(p) == 'lib.py' and not ln.lstrip().startswith('#'):
                marked += 1; continue
            for pat in BANNED:
                if re.search(pat, ln):
                    hits.append('%s:%d: %s' % (os.path.basename(p), i+1, ln.strip())); break
    return hits, marked

def test_no_stray_random():
    hits, marked = stray_random_sources(HERE)
    assert not hits, 'stray random or time sources: ' + '; '.join(hits)
    assert marked == 1, 'expected exactly one %s line in lib.py (the rng function), found %d' % (ALLOW, marked)

TESTS = [test_tempo_and_key, test_melody_and_bass, test_vibrato, test_rng_deterministic, test_no_stray_random]

if __name__ == '__main__':
    bad = 0
    for t in TESTS:
        try:
            t(); print('PASS', t.__name__)
        except AssertionError as e:
            bad += 1; print('FAIL', t.__name__, '-', e)
        except Exception:
            bad += 1; print('ERROR', t.__name__); traceback.print_exc()
    sys.exit(1 if bad else 0)
