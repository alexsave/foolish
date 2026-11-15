# test_extract.py - helper of test_extract.sh: builds the synthetic song, the exact-flag fixtures, and checks the renders.
#
# What it does: `make DIR` writes a 12 s synthetic song (test_compare's builder, swung and jittered off the grid) as drums.wav,
#   bass.wav, other.wav and mix.wav; `exactmake DIR` writes a tone sample and two replica directories (exact 1 and exact 0);
#   `check DIR` compares the replica renders with the song and asserts; `exactcheck DIR` asserts the exact flag.
# Inputs: a directory. Outputs: numbers on stdout, one FAIL line per broken assertion, exit code 1 on any failure.
# Randomness: ONE generator, test_compare's, seeded by its SEED; this file draws nothing of its own.
import os, sys
import numpy as np
from scipy.io import wavfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare as C
import test_compare as T

SR = 44100
BPM = 120.0
SECONDS = T.DUR
SEED = T.SEED          # the one seed (test_compare's); the synthetic song and its jitter derive from it


def dev_swing(step):
    """ms: off 16ths late by 40 and 34 ms (beyond the 30 ms onset tolerance, so a grid-rounded replica misses them), plus jitter of +-4 ms. Never on a grid."""
    return [0.0, 40.0, 0.0, 34.0][step % 4] + (T.JIT[step] - 0.5) * 8


def write(path, x):
    wavfile.write(path, SR, np.stack([x, x], axis=1).astype(np.float32))


_KICK = T._kick
STAB_STEPS = [0, 3, 6, 10]      # the pad is chord stabs: on these 16ths of every bar, a 5 ms attack, then a 150 ms decay time
BASS_DECAY = 0.2                # the bass is plucked: a 5 ms attack and a 200 ms decay time from each of the builder's own bass steps


def gate(n, steps, decay):
    """Amplitude gate that turns the builder's stationary pad and legato bass into stabs and plucks on swung, off-grid times (a
    stationary signal has no onsets and a loudness curve flat to 0.5 dB, which no replica can correlate with)."""
    sd = 60.0 / BPM / 4
    ts = sorted((b * 16 + st) * sd + dev_swing(b * 16 + st) / 1000.0 for b in range(int(SECONDS / (16 * sd)) + 1) for st in steps)
    g = np.zeros(n)
    for k, t0 in enumerate(ts):
        i0 = int(round(t0 * SR)); i1 = int(round(ts[k + 1] * SR)) if k + 1 < len(ts) else n
        if i0 >= n:
            break
        i1 = min(i1, n)
        t = np.arange(i1 - i0) / SR
        g[i0:i1] = np.minimum(1, t / 0.005) * np.exp(-t / decay)
    return g


def _kick_with_click(vel):
    """test_compare's sine-sweep kick plus a 3 ms click. The onset detector (an energy rise across many bins) cannot see a bare
    sub-bass sweep next to hats; a real kick has a click, and a kit the detector cannot see cannot be segmented."""
    y = _KICK(vel)
    n = int(0.003 * SR)
    y[:n] += 0.5 * vel * np.hanning(2 * n)[n:] * T._hp(T._N[:n], 3000)
    return y


T._kick = _kick_with_click


def make(d):
    os.makedirs(d, exist_ok=True)
    s = T.make_song(bpm=BPM, dev=dev_swing, prog=T.PROG_A[:1])   # one chord, so the bass has four pitches (root, fifth, 7th, octave) that a few samples can cover
    s['other'] = s['other'] * gate(len(s['other']), STAB_STEPS, 0.15)
    s['bass'] = s['bass'] * gate(len(s['bass']), T.PAT_A['bass'], BASS_DECAY)
    s['mix'] = s['drums'] + s['bass'] + s['other']
    for k in ('drums', 'bass', 'other', 'mix'):
        write(os.path.join(d, k + '.wav'), s[k])


FAILS = []


def check(name, ok, msg):
    print('  %s %s: %s' % ('ok  ' if ok else 'FAIL', name, msg))
    if not ok:
        FAILS.append(name)


def dec(p):
    return C.decode(p, SR)[0]


def do_check(d):
    o = {k: dec(os.path.join(d, 'orig', k + '.wav')) for k in ('mix', 'drums', 'bass', 'other')}
    r = {'mix': dec(os.path.join(d, 'rep', 'out', 'full.wav'))}
    for k in ('drums', 'bass', 'other'):
        r[k] = dec(os.path.join(d, 'rep', 'solo_' + k, 'full_solo.wav'))
    R = C.compare(o['mix'], r['mix'], SR, {k: o[k] for k in ('drums', 'bass', 'other')}, {k: r[k] for k in ('drums', 'bass', 'other')})
    print('  overall %.2f, offset_ms %.2f, drift_ms %.2f, mix onset F@30 %.3f, mix onset dev mean %.2f ms, drums F@30 %.3f'
          % (R['overall'], R['offset_ms'], R['drift_ms'], R['mix.onset_f30'], R['mix.onset_dev_mean_ms'], R['drums.onset_f30']))
    print('  terms: ' + ', '.join('%s %s' % (k, C.fmt(v)) for k, v in R['_extra']['terms']))
    check('overall', R['overall'] >= 90, 'overall = %.2f, expected >= 90' % R['overall'])
    check('mix_onset_f30', R['mix.onset_f30'] >= 0.95, 'mix onset F@30 = %.3f, expected >= 0.95' % R['mix.onset_f30'])
    check('onset_mean_dev', abs(R['mix.onset_dev_mean_ms']) <= 3, 'mean signed onset deviation = %.2f ms, expected |.| <= 3' % R['mix.onset_dev_mean_ms'])
    check('constant_offset', abs(R['offset_ms']) <= 4, 'constant offset = %.2f ms (replica late is positive), expected |.| <= 4' % R['offset_ms'])
    check('drums_onset_f30', R['drums.onset_f30'] >= 0.95, 'drums-stem onset F@30 = %.3f, expected >= 0.95' % R['drums.onset_f30'])
    for k in ('drums', 'bass', 'other'):
        v = R[k + '.loud400_corr']
        check('loud400_' + k, v >= 0.9, '%s 400 ms loudness correlation = %.3f, expected >= 0.9' % (k, v))
    ns = len([f for f in os.listdir(os.path.join(d, 'rep', 'samples_drums')) if f.endswith('.wav')])
    check('drum_samples', ns <= 8, 'drum samples = %d, expected <= 8' % ns)
    return R


def rms(x):
    return float(np.sqrt(np.mean(x ** 2)))


def first_sound_ms(path):
    import extract as X
    sr, x = wavfile.read(path)
    return 1000.0 * X.attack_index(x.astype(np.float64), sr) / sr


def leadmake(d):
    """Three isolated noise bursts in silence (2 s): nothing but the onset detector decides how much precedes each attack."""
    os.makedirs(d, exist_ok=True)
    x = np.zeros(2 * SR)
    for t0 in (0.3, 0.9, 1.5):
        n = int(0.08 * SR); i = int(t0 * SR)
        x[i:i + n] = 0.5 * T._hp(T._N[:n], 2000) * np.exp(-np.arange(n) / SR / 0.02) * np.minimum(1, np.arange(n) / (0.001 * SR))
    write(os.path.join(d, 'bursts.wav'), x)


def leadcheck(d):
    """--lead-ms is the pre-roll kept before each onset: the first burst's sample holds 6 ms more material ahead of its attack with
    --lead-ms 6 than with --lead-ms 0."""
    a = first_sound_ms(os.path.join(d, 'lead6', 'samples', 'B_000.wav'))
    b = first_sound_ms(os.path.join(d, 'lead12', 'samples', 'B_000.wav'))
    check('lead', abs((b - a) - 6.0) <= 0.3, 'attack position with --lead-ms 12 minus with 6 = %.2f ms (%.2f - %.2f), expected 6.00 +-0.3' % (b - a, b, a))


N_EVENTS = 10
TONE_A = 0.8


def exactmake(d):
    os.makedirs(d, exist_ok=True)
    n = int(0.3 * SR); t = np.arange(n) / SR
    x = TONE_A * np.sin(2 * np.pi * 440 * t)
    a, b = int(0.002 * SR), int(0.008 * SR)
    x[:a] *= np.linspace(0, 1, a); x[n - b:] *= np.linspace(1, 0, b)
    write(os.path.join(d, 'tone.wav'), x)
    man = os.path.join(d, 'tone_man.txt')
    open(man, 'w').write('60 0 0 %s\n' % os.path.abspath(os.path.join(d, 'tone.wav')))
    ev = ' '.join('%.4f,60,1.0000,0.4500' % (1.0 + 2.0 * k) for k in range(N_EVENTS))
    for ex in (1, 0):
        dd = os.path.join(d, 'exact%d' % ex)
        os.makedirs(dd, exist_ok=True)
        open(os.path.join(dd, 'score.txt'), 'w').write('TEMPO 120\nSECTION E BARS 6 KEY C\nEVENTS X: %s\n' % ev)
        open(os.path.join(dd, 'parts.txt'), 'w').write('PART X sampler %s gain_db 0 release_s 0.012 group other exact %d\n' % (man, ex))


def exactcheck(d):
    sr, tone = wavfile.read(os.path.join(d, 'tone.wav'))
    ref = rms(tone[int(0.05 * sr):int(0.25 * sr), 0])
    out = {}
    for ex in (1, 0):
        sr2, y = wavfile.read(os.path.join(d, 'exact%d' % ex, 'out', 'full_solo.wav'))
        y = y[:, 0] / (2 ** 31 if y.dtype == np.int32 else 32768.0 if y.dtype == np.int16 else 1.0)
        devs = []
        for k in range(N_EVENTS):
            t0 = (1.0 + 2.0 * k) * 0.5
            seg = y[int((t0 + 0.05) * sr2):int((t0 + 0.25) * sr2)]
            devs.append(20 * np.log10(rms(seg) / ref))
        out[ex] = np.array(devs)
        print('  exact %d: levels vs stored sample (dB): %s' % (ex, ' '.join('%.3f' % v for v in devs)))
    want = 20 * np.log10(0.5)
    check('exact_level', bool(np.all(np.abs(out[1] - want) <= 0.05)), 'with exact: worst deviation from %.2f dB = %.3f dB, expected <= 0.05' % (want, np.max(np.abs(out[1] - want))))
    check('exact_off_varies', float(np.max(np.abs(out[0] - want))) > 0.2, 'without exact: worst deviation = %.3f dB, expected > 0.2 (the random gain is on)' % np.max(np.abs(out[0] - want)))


if __name__ == '__main__':
    cmd, d = sys.argv[1], sys.argv[2]
    {'make': make, 'exactmake': exactmake, 'check': do_check, 'exactcheck': exactcheck, 'lead': leadcheck, 'leadmake': leadmake}[cmd](d)
    if FAILS:
        print('FAILED: ' + ', '.join(FAILS))
        sys.exit(1)
