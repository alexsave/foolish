# test_compare.py - tests for compare.py on synthetic songs.
#
# What it does: builds a deterministic synthetic song (kick, claps, hats, bass, pads; mix and stems), makes
#   deliberately wrong replicas of it, and asserts that compare.py measures each flaw.
# Inputs: none. Outputs: one line per test with the measured numbers, then PASS/FAIL; exit code 1 on any failure.
# How to run: python3 -I test_compare.py
# Known failure modes: thresholds are tuned to these synthetic signals, not to real music.
# Randomness: ONE generator, rng() below, seeded by SEED (the single named constant). Nothing else is random.
import os, sys, traceback
import numpy as np
from scipy import signal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare as C

SEED = 20261115   # the one seed; change this line to fix every random draw of this file
SR = 44100
DUR = 12.0
BPM = 90.0


def rng():
    return np.random.default_rng(SEED)


_R = rng()
_N = _R.standard_normal(SR * 2)
JIT = _R.random(4000)                  # per-step jitter draws (uniform 0..1)
VEL = _R.random(400)                   # per-hit velocity draws


def _bp(x, lo, hi, order=4):
    sos = signal.butter(order, [lo, hi], 'band', fs=SR, output='sos')
    y = signal.sosfilt(sos, x)
    return y / y.std()


def _hp(x, f, order=6):
    sos = signal.butter(order, f, 'high', fs=SR, output='sos')
    y = signal.sosfilt(sos, x)
    return y / y.std()


CLAP_N = _bp(_N, 1500, 5000, order=8)   # steep, so the claps leave the hat band alone
HAT_N = _hp(_N, 8000)

PROG_A = [(33, [57, 60, 64]), (29, [57, 60, 65]), (36, [55, 60, 64]), (31, [55, 59, 62]), (38, [57, 62, 65]), (40, [56, 59, 64])]
PROG_B = [(40, [56, 59, 64]), (38, [57, 62, 65]), (31, [55, 59, 62]), (36, [55, 60, 64]), (29, [57, 60, 65]), (33, [57, 60, 64])]
PAT_A = dict(kick=[0, 4, 8, 12], kick_odd=[10], clap=[4, 12], hat=[1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15], bass=[2, 5, 7, 10, 13, 15])
PAT_B = dict(kick=[0, 3, 6, 10, 14], clap=[2, 9, 13], hat=[0, 1, 4, 5, 8, 11, 12], bass=[1, 4, 8, 11, 14])


def dev_base(step):      # slightly swung, jittered
    return [0.0, 6.0, 0.0, 10.0][step % 4] + (JIT[step] - 0.5) * 4


def dev_wild(step):      # 35-60 ms off the grid, fixed sign per slot
    s = [0.0, 1.0, 1.0, -1.0][step % 4]
    return s * (35.0 + 25.0 * JIT[step])


def dev_zero(step):
    return 0.0


def _shape(n, att, rel):
    """Raised-cosine attack and release, so the synthetic drums leave no broadband click in the hat band."""
    t = np.arange(n) / SR
    a = np.where(t < att, 0.5 * (1 - np.cos(np.pi * np.minimum(t, att) / att)), 1.0)
    r = np.where(t > n / SR - rel, 0.5 * (1 + np.cos(np.pi * (t - (n / SR - rel)) / rel)), 1.0)
    return a * r


def _kick(vel):
    n = int(0.3 * SR); t = np.arange(n) / SR
    ph = 2 * np.pi * np.cumsum(45 + 75 * np.exp(-t / 0.03)) / SR
    return 0.8 * vel * np.sin(ph) * np.exp(-t / 0.1) * _shape(n, 0.006, 0.05)


def _clap():
    n = int(0.2 * SR); t = np.arange(n) / SR
    return 0.3 * CLAP_N[:n] * np.exp(-t / 0.05) * _shape(n, 0.003, 0.05)


def _hat(k):
    n = int(0.06 * SR); t = np.arange(n) / SR
    o = (k * 3571) % (len(HAT_N) - n)
    return 0.12 * HAT_N[o:o + n] * np.exp(-t / 0.012) * _shape(n, 0.0005, 0.01)


def _saw_note(midi, dur, lp, amp, att, rel):
    n = int(dur * SR); t = np.arange(n) / SR
    f = 440 * 2 ** ((midi - 69) / 12)
    y = signal.sawtooth(2 * np.pi * f * t)
    sos = signal.butter(2, lp, 'low', fs=SR, output='sos')
    y = signal.sosfilt(sos, y)
    env = np.minimum(1, t / att) * np.minimum(1, (dur - t) / rel)
    return amp * y * env


def _add(buf, y, t0):
    i = int(round(t0 * SR))
    if i < 0 or i >= len(buf):
        return
    j = min(len(buf), i + len(y))
    buf[i:j] += y[:j - i]


def make_song(bpm=BPM, transpose=0, dev=dev_base, hats=True, kick_gain=None, duck_db=0.0, pat=PAT_A, prog=PROG_A, vel_var=True):
    """Returns dict(mix=..., drums=..., bass=..., other=..., kicks=[kick times])."""
    n = int(DUR * SR)
    sd = 60.0 / bpm / 4
    nsteps = int(DUR / sd)
    drums, bass, other = np.zeros(n), np.zeros(n), np.zeros(n)
    kicks = []
    ki = 0; hi_ = 0
    for s in range(nsteps):
        p = s % 16
        t = s * sd + dev(s) / 1000.0
        if p in pat['kick'] or (p in pat.get('kick_odd', []) and (s // 16) % 2 == 1):
            vel = (0.75 + 0.25 * VEL[ki]) if vel_var else 1.0
            if kick_gain is not None:
                vel *= kick_gain(ki)
            _add(drums, _kick(vel), t); kicks.append(t); ki += 1
        if p in pat['clap']:
            _add(drums, _clap(), t)
        if hats and p in pat['hat'] and JIT[(s * 13 + 5) % 4000] < 0.8:   # ~20% of the hats are left out
            _add(drums, _hat(hi_), t); hi_ += 1
    # bass: legato notes, flat level, until the next note
    nb = len(pat['bass'])
    notes = []
    for bar in range(int(DUR / (16 * sd)) + 1):
        root = prog[bar % len(prog)][0] + transpose
        for k, st in enumerate(pat['bass']):
            notes.append((bar * 16 + st, root + [0, 0, 12, 7, 0, 10, 12][k]))
    for i, (st, m) in enumerate(notes):
        t0 = st * sd + dev(st) / 1000.0
        t1 = (notes[i + 1][0] * sd + dev(notes[i + 1][0]) / 1000.0) if i + 1 < len(notes) else DUR
        if t0 >= DUR:
            break
        _add(bass, _saw_note(m, min(t1, DUR + 0.5) - t0 + 0.005, 600, 0.3, 0.005, 0.005), t0)
    # pads
    for bar in range(int(DUR / (16 * sd)) + 1):
        t0 = bar * 16 * sd
        if t0 >= DUR:
            break
        for m in prog[bar % len(prog)][1]:
            _add(other, _saw_note(m + transpose, 16 * sd + 0.1, 2500, 0.05, 0.15, 0.1), t0)
    if duck_db:
        g = np.ones(n)
        for tk in kicks:
            i0 = int(tk * SR); i1 = min(n, i0 + int(0.15 * SR))
            if i0 >= n:
                continue
            ramp = np.arange(i1 - i0) / (0.15 * SR)
            g[i0:i1] = np.minimum(g[i0:i1], 10 ** (-duck_db * (1 - ramp) / 20))
        bass, other = bass * g, other * g
    return dict(mix=drums + bass + other, drums=drums, bass=bass, other=other, kicks=kicks)


def stems_of(s):
    return {k: s[k] for k in ('drums', 'bass', 'other')}


def run(o, r, align=True, stems=True):
    return C.compare(o['mix'], r['mix'], SR, stems_of(o) if stems else None, stems_of(r) if stems else None, align=align)


def apply(s, fn):
    out = {k: fn(s[k]) for k in ('mix', 'drums', 'bass', 'other')}
    out['kicks'] = s['kicks']
    return out


def delayed(s, sec):
    k = int(round(sec * SR))
    return apply(s, lambda x: np.concatenate([np.zeros(k), x]))


def lowpassed(s, f):
    sos = signal.butter(4, f, 'low', fs=SR, output='sos')
    return apply(s, lambda x: signal.sosfilt(sos, x))


# ---------------------------------------------------------------- tests
def show(name, **kv):
    print('  %s: ' % name + ', '.join('%s=%s' % (k, C.fmt(v)) for k, v in kv.items()))


def test_a_identical():
    s = make_song()
    R = run(s, s)
    fkeys = [k for k in R if not k.startswith('_') and (k.endswith('onset_f15') or k.endswith('onset_f30') or k.endswith('onset_f50') or k.endswith('_f30'))]
    for k in ('mix.onset_f30', 'drums.onset_f30', 'bass.onset_f30', 'other.onset_f30', 'drums.kick_f30', 'drums.clap_f30', 'drums.hat_f30'):
        assert k in fkeys, 'identical: missing F key %s' % k
    for k in fkeys:
        assert R[k] == 1.0, 'identical: %s = %s, expected 1' % (k, R[k])
    assert R['mix.chroma_cos'] > 0.999, 'identical: chroma_cos = %s' % R['mix.chroma_cos']
    assert R['mix.lts_mad_db'] < 0.1, 'identical: lts_mad_db = %s' % R['mix.lts_mad_db']
    assert R['overall'] >= 99.5, 'identical: overall = %s' % R['overall']
    show('a', overall=R['overall'], n_mix_onsets=R['mix.onset_n_orig'], kick=R['drums.kick_n_orig'], clap=R['drums.clap_n_orig'], hat=R['drums.hat_n_orig'], chroma_chance=R['mix.chroma_cos_chance'])


def test_b_delay():
    s = make_song(); r = delayed(s, 0.120)
    Ra = run(s, r); Rn = run(s, r, align=False)
    assert abs(Ra['offset_ms'] - 120) <= 3, 'delay: offset_ms = %s, expected 120 +-3' % Ra['offset_ms']
    assert Ra['mix.onset_f30'] >= 0.98, 'delay aligned: mix onset_f30 = %s, expected >= 0.98' % Ra['mix.onset_f30']
    assert Rn['mix.onset_f30'] <= 0.2, 'delay unaligned: mix onset_f30 = %s, expected <= 0.2' % Rn['mix.onset_f30']
    show('b', offset_ms=Ra['offset_ms'], f30_aligned=Ra['mix.onset_f30'], f30_unaligned=Rn['mix.onset_f30'])


def test_c_quantised():
    o = make_song(dev=dev_wild); q = make_song(dev=dev_zero)
    Rq = run(o, q); Ri = run(o, o)
    assert Rq['mix.onset_f30'] < 0.6, 'quantised: mix onset_f30 = %s, expected < 0.6' % Rq['mix.onset_f30']
    assert Ri['mix.onset_f30'] - Rq['mix.onset_f30'] > 0.4, 'quantised: F drop vs identical only %s' % (Ri['mix.onset_f30'] - Rq['mix.onset_f30'])
    assert Rq['mix.microtiming_profile_mad_ms'] > 20, 'quantised: microtiming_profile_mad_ms = %s, expected > 20' % Rq['mix.microtiming_profile_mad_ms']
    show('c', f15=Rq['mix.onset_f15'], f30=Rq['mix.onset_f30'], f50=Rq['mix.onset_f50'], micro_mad=Rq['mix.microtiming_profile_mad_ms'],
         offset_ms=Rq['offset_ms'], dev_mean=Rq['mix.onset_dev_mean_ms'], dev_std=Rq['mix.onset_dev_std_ms'])


def test_d_transposed():
    s = make_song(); t = make_song(transpose=1)
    R = run(s, t)
    assert R['mix.onset_f30'] >= 0.95, 'transposed: mix onset_f30 = %s, expected >= 0.95' % R['mix.onset_f30']
    assert R['mix.chroma_cos_above_chance'] < 0.5, 'transposed: chroma_cos_above_chance = %s, expected < 0.5' % R['mix.chroma_cos_above_chance']
    assert R['bass.pitch_within50'] < 0.1, 'transposed: bass pitch_within50 = %s, expected < 0.1' % R['bass.pitch_within50']
    show('d', f30=R['mix.onset_f30'], chroma_cos=R['mix.chroma_cos'], chance=R['mix.chroma_cos_chance'], above=R['mix.chroma_cos_above_chance'],
         bass50=R['bass.pitch_within50'], bass50oct=R['bass.pitch_within50_octave'], overlap=R['bass.pitch_voiced_overlap'])


def test_e_no_hats():
    s = make_song(); r = make_song(hats=False)
    R = run(s, r)
    assert R['drums.hat_recall'] < 0.1, 'no hats: hat_recall = %s, expected < 0.1' % R['drums.hat_recall']
    assert R['drums.kick_f30'] >= 0.95, 'no hats: kick_f30 = %s, expected >= 0.95' % R['drums.kick_f30']
    show('e', hat_recall=R['drums.hat_recall'], hat_n_orig=R['drums.hat_n_orig'], hat_n_replica=R['drums.hat_n_replica'], kick_f30=R['drums.kick_f30'], overall=R['overall'])


def test_f_kick_strength():
    s = make_song(); r = make_song(kick_gain=lambda i: 0.5 if i % 2 else 1.0)
    R = run(s, r)
    assert R['drums.kick_f30'] >= 0.95, 'half kicks: kick_f30 = %s, expected >= 0.95' % R['drums.kick_f30']
    assert R['drums.kick_strength_absdiff_db'] > 2.0, 'half kicks: kick_strength_absdiff_db = %s, expected > 2' % R['drums.kick_strength_absdiff_db']
    assert R['drums.kick_strength_corr'] < 0.7, 'half kicks: kick_strength_corr = %s, expected < 0.7' % R['drums.kick_strength_corr']
    Ri = run(s, s)
    assert Ri['drums.kick_strength_absdiff_db'] < 0.01, 'identical kick_strength_absdiff_db = %s' % Ri['drums.kick_strength_absdiff_db']
    show('f', kick_f30=R['drums.kick_f30'], corr=R['drums.kick_strength_corr'], absdiff_db=R['drums.kick_strength_absdiff_db'])


def test_g_lowpass():
    s = make_song(); r = lowpassed(s, 2000)
    R = run(s, r)
    assert R['mix.bright_median_ratio'] < 0.8, 'lowpass: bright_median_ratio = %s, expected < 0.8' % R['mix.bright_median_ratio']
    assert R['mix.lts_mad_db'] > 3, 'lowpass: lts_mad_db = %s, expected > 3' % R['mix.lts_mad_db']
    show('g', bright_ratio=R['mix.bright_median_ratio'], bright_corr=R['mix.bright_corr'], lts_mad=R['mix.lts_mad_db'], overall=R['overall'])


def test_h_faster():
    s = make_song(); r = make_song(bpm=BPM * 1.03)
    R = run(s, r)
    pd = R['mix.tempo_pct_diff']
    assert 2.0 <= abs(pd) <= 4.0, 'faster: tempo_pct_diff = %s, expected 2..4' % pd
    assert abs(R['drift_ms']) > 100, 'faster: drift_ms = %s, expected |.| > 100' % R['drift_ms']
    assert R['mix.onset_f30'] < 0.5, 'faster: mix onset_f30 = %s, expected < 0.5' % R['mix.onset_f30']
    show('h', tempo_orig=R['mix.tempo_bpm_orig'], tempo_rep=R['mix.tempo_bpm_replica'], pct=pd, drift_ms=R['drift_ms'], offset_ms=R['offset_ms'], f30=R['mix.onset_f30'])


def test_i_sidechain():
    o = make_song(duck_db=8.0); r = make_song()
    R = run(o, r)
    # expected pump: window 20-80 ms after the kick, gain ramps linearly in dB from -8 at 0 to 0 at 150 ms;
    # the window level is the RMS (power mean) of that gain, relative to 0 dB at 150-250 ms.
    t = np.linspace(0.02, 0.08, 601)
    exp = 10 * np.log10(np.mean(10 ** (2 * (-8.0 * (1 - t / 0.15)) / 20)))
    assert abs(R['mix.pump_db_orig'] - exp) <= 2.0, 'sidechain: pump_db_orig = %s, expected %.2f +-2' % (R['mix.pump_db_orig'], exp)
    assert R['mix.pump_db_replica'] > -1.5, 'sidechain: pump_db_replica = %s, expected > -1.5' % R['mix.pump_db_replica']
    assert R['mix.pump_recovery_ms_orig'] > 60, 'sidechain: pump_recovery_ms_orig = %s, expected > 60' % R['mix.pump_recovery_ms_orig']
    show('i', expected=exp, pump_orig=R['mix.pump_db_orig'], pump_replica=R['mix.pump_db_replica'], rec_orig=R['mix.pump_recovery_ms_orig'],
         rec_replica=R['mix.pump_recovery_ms_replica'], loud20_corr=R['mix.loud20_corr'])


def test_j_unrelated():
    s = make_song(); u = make_song(bpm=97.0, transpose=6, pat=PAT_B, prog=PROG_B, dev=dev_zero)
    R = run(s, u)
    assert R['overall'] < 35, 'unrelated: overall = %s, expected < 35' % R['overall']
    assert R['mix.chroma_cos_above_chance'] < 0.3, 'unrelated: chroma_cos_above_chance = %s, expected < 0.3' % R['mix.chroma_cos_above_chance']
    show('j', overall=R['overall'], chroma_above=R['mix.chroma_cos_above_chance'], cqt_above=R['mix.cqt_cos_above_chance'], f30=R['mix.onset_f30'])


def test_k_gain():
    s = make_song(); r = apply(s, lambda x: 0.4 * x)
    R = run(s, r)
    assert R['overall'] >= 99.5, 'gain: overall = %s, expected >= 99.5' % R['overall']
    assert R['mix.loud400_mad_db'] < 0.1, 'gain: loud400_mad_db = %s, expected < 0.1 (levels are normalised)' % R['mix.loud400_mad_db']
    show('k', overall=R['overall'], loud_mad=R['mix.loud400_mad_db'])


def test_l_robust():
    s = make_song()
    # stereo input, replica 1 s shorter, silent vocals stem, then no stems at all
    st = np.stack([s['mix'], s['mix']], axis=1)
    n = int((DUR - 1) * SR)
    so = stems_of(s); so['vocals'] = np.zeros(len(s['mix']))
    sr_ = {k: v[:n] for k, v in so.items()}
    R = C.compare(st, s['mix'][:n], SR, so, sr_)
    assert R['overall'] >= 99.0, 'robust: stereo/shorter/silent-vocals overall = %s' % R['overall']
    assert 'vocals' in R['_extra']['silent'], 'robust: silent vocals stem not reported'
    R2 = C.compare(s['mix'], s['mix'], SR)
    assert R2['overall'] >= 99.0, 'robust: no stems overall = %s' % R2['overall']
    assert R2['mix.pump_db_orig'] != R2['mix.pump_db_orig'], 'robust: pump should be n/a without stems'
    z = dict(drums=np.zeros(len(s['mix'])), bass=s['bass'], other=s['other'])
    R3 = C.compare(s['mix'], s['mix'], SR, z, z)
    assert R3['overall'] == R3['overall'], 'robust: silent drums stem broke the score'
    show('l', stereo_overall=R['overall'], nostems_overall=R2['overall'], silent_drums_overall=R3['overall'])


TESTS = [test_a_identical, test_b_delay, test_c_quantised, test_d_transposed, test_e_no_hats, test_f_kick_strength,
         test_g_lowpass, test_h_faster, test_i_sidechain, test_j_unrelated, test_k_gain, test_l_robust]


def main():
    bad = 0
    sel = [t for t in TESTS if len(sys.argv) < 2 or t.__name__.split('_')[1] in sys.argv[1:]]
    for t in sel:
        try:
            t()
            print('PASS %s' % t.__name__)
        except AssertionError as e:
            bad += 1
            print('FAIL %s: %s' % (t.__name__, e))
        except Exception:
            bad += 1
            print('ERROR %s' % t.__name__)
            traceback.print_exc()
    print('%d of %d tests failed' % (bad, len(sel)))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
