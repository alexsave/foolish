#!/bin/bash
# test_glue.sh: glue.c (the tiny synth for bridging passages) on its own scores, self-cleaning.
# One PASS or FAIL line per check, non-zero exit if any check fails. Every check was mutation checked (the code it covers
# broken, the named check seen to fail, the code restored).
#   GLUE_C=/path/to/copy.c  test a (mutated) copy of glue.c instead of the real one
#   PYTHON=/path/to/python  the python with numpy (default python3)
#   ONLY=text                 run only the checks whose name contains the text
PY="${PYTHON:-python3}"
D="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
SRC_C="${GLUE_C:-$D/glue.c}"
clang -O2 -Wall -Werror -I"$D" -o "$T/glue" "$SRC_C" 2> "$T/cc.log" || { cat "$T/cc.log"; echo "FAIL build"; exit 1; }
[ -s "$T/cc.log" ] && { cat "$T/cc.log"; echo "FAIL build warnings"; exit 1; }
cat > "$T/t.py" <<'PY'
import numpy as np, subprocess, os
SR = 48000
T = os.environ['T']; BIN = T + '/glue'; ONLY = os.environ.get('ONLY', '')
fails = []

def run(lines, tag='g', raw=False):
    sp = f'{T}/{tag}.score'; open(sp, 'w').write('\n'.join(lines) + '\n')
    o = f'{T}/{tag}.wav'
    if os.path.exists(o): os.remove(o)
    r = subprocess.run([BIN, sp, o], capture_output=True, text=True)
    class R: pass
    R.rc, R.err, R.path = r.returncode, r.stderr, o
    R.y = None
    if os.path.exists(o):
        R.bytes = open(o, 'rb').read()
        d = subprocess.run(['ffmpeg', '-v', 'error', '-nostdin', '-i', o, '-f', 'f32le', '-ar', '48000', '-ac', '2', '-'], capture_output=True, check=True)
        R.y = np.frombuffer(d.stdout, dtype='<f4').reshape(-1, 2).astype(np.float64)
    return R
def db(x): return 20 * np.log10(abs(x) + 1e-12)
def rms(x): return float(np.sqrt(np.mean(np.square(x))))
def seg(x, a, b): return x[int(round(a * SR)):int(round(b * SR))]
def spec(x):
    w = np.hanning(len(x)); return np.fft.rfftfreq(len(x), 1 / SR), np.abs(np.fft.rfft(x * w)) ** 2
def check(name, fn):
    if ONLY and ONLY not in name: return
    try: ok, detail = fn()
    except Exception as e: ok, detail = False, 'exception %r' % (e,)
    if not ok: fails.append(name)
    print(('PASS ' if ok else 'FAIL ') + name + (' - ' + detail if detail else '')); sys.stdout.flush()
import sys

DRY = 'REVERB -120'
def pad_hz():
    R = run(['TEMPO 60', 'LEN 6', DRY, 'PAD 0 6 57 -10'], 'p'); m = R.y.mean(axis=1)
    f, P = spec(seg(m, 1.0, 5.0)); k = (f > 100) & (f < 500); peak = f[k][np.argmax(P[k])]
    low = P[(f > 200) & (f < 240)].sum(); hi = P[f > 3600].sum()
    return R.rc == 0 and abs(peak - 220) < 2.2 and 10 * np.log10(hi / low) < -30, f'peak {peak:.2f} Hz, above 3600 Hz {10 * np.log10(hi / low):.1f} dB under the 200-240 Hz band'
check('PAD at midi 57 sits at 220 Hz with nothing above twice the cutoff', pad_hz)
def pad_cut():
    R = run(['TEMPO 60', 'LEN 6', DRY, 'CUTOFF 600', 'PAD 0 6 57 -10'], 'pc'); m = R.y.mean(axis=1)
    f, P = spec(seg(m, 1.0, 5.0)); low = P[(f > 200) & (f < 240)].sum(); hi = P[f > 1200 * 1.15].sum()
    return R.rc == 0 and 10 * np.log10(hi / low) < -30, f'above 1380 Hz with CUTOFF 600: {10 * np.log10(hi / low):.1f} dB under'
check('CUTOFF moves the pad low-pass', pad_cut)
def pad_attack():
    R = run(['TEMPO 60', 'LEN 6', DRY, 'PAD 0 6 57 -10'], 'pa'); m = R.y.mean(axis=1)
    a = db(rms(seg(m, 0.04, 0.06))); b = db(rms(seg(m, 0.55, 0.65)))
    return R.rc == 0 and b - a > 12, f'50 ms is {b - a:.1f} dB under 600 ms'
check('PAD attacks over 0.35 s', pad_attack)
def keys_decay():
    R = run(['TEMPO 60', 'LEN 5', DRY, 'KEYS 0 5 60 -10'], 'k'); m = R.y.mean(axis=1)
    a = db(rms(seg(m, 0.05, 0.15))); b = db(rms(seg(m, 1.45, 1.55)))
    return R.rc == 0 and a - b > 2.0, f'1.5 s is {a - b:.2f} dB under 0.1 s (exp(-1.4 / 2.2) is 5.5 dB, less the 2.3 dB the FM waveform gains in RMS as its index falls)'
check('KEYS decays while held', keys_decay)
def keys_tremolo():
    R = run(['TEMPO 60', 'LEN 5', DRY, 'KEYS 0 5 60 -10'], 'kt'); x = R.y[:, 0]; y = R.y[:, 1]
    w, h = int(0.02 * SR), int(0.01 * SR); st = np.arange(int(0.9 * SR), int(3.9 * SR) - w, h)
    r = np.array([db(rms(x[i:i + w])) - db(rms(y[i:i + w])) for i in st]); r -= r.mean()
    f = np.fft.rfftfreq(len(r), 0.01); pk = f[np.argmax(np.abs(np.fft.rfft(r))[1:]) + 1]; amp = (r.max() - r.min()) / 2
    return R.rc == 0 and 4.0 < pk < 5.0 and 2.4 < amp < 3.6, f'L over R swings {amp:.2f} dB at {pk:.2f} Hz (+-1.5 dB opposite in the two is +-3 dB between them)'
check('KEYS tremolo is opposite in L and R', keys_tremolo)
def sub_note():
    R = run(['TEMPO 60', 'LEN 3', DRY, 'SUB 0 3 33 -10'], 's'); L, Rr = R.y[:, 0], R.y[:, 1]
    f, P = spec(seg(L, 0.5, 2.5)); k = (f > 20) & (f < 400); peak = f[k][np.argmax(P[k])]
    d = float(np.max(np.abs(L - Rr)))
    return R.rc == 0 and abs(peak - 55) < 1.0 and d < 1e-5, f'peak {peak:.2f} Hz, max |L - R| {d:.2e}'
check('SUB at midi 33 peaks at 55 Hz and is mono', sub_note)
def timing():
    R = run(['TEMPO 120', 'LEN 8', DRY, 'KEYS 4 1 69 -10'], 'tm'); m = np.abs(R.y.mean(axis=1))
    on = int(np.argmax(m > 0.01 * m.max())) / SR
    return R.rc == 0 and abs(on - 2.0) < 0.005, f'starts at {on:.4f} s'
check('a note at beat 4 of TEMPO 120 starts at 2.000 s', timing)
def determinism():
    sc = ['TEMPO 90', 'LEN 4', 'PAD 0 4 57,60,64 -10', 'KEYS 1 2 72 -12', 'SUB 0 4 33 -12']
    a = run(sc, 'd1'); b = run(sc, 'd2'); c = run(sc + ['SEED 7'], 'd3')
    return a.rc == 0 and a.bytes == b.bytes and c.bytes != a.bytes, f'same {a.bytes == b.bytes}, other seed differs {c.bytes != a.bytes}'
check('two renders are byte-identical and SEED changes the bytes', determinism)
def errors():
    cases = {'no TEMPO': ['LEN 4', 'PAD 0 4 57 -10'], 'TEMPO not first': ['LEN 4', 'TEMPO 120'], 'unknown statement': ['TEMPO 120', 'WOBBLE 1'],
             'bad number': ['TEMPO 120', 'PAD 0 x 57 -10'], 'trailing junk': ['TEMPO 120', 'KEYS 0 1x 60 -10'], 'nan': ['TEMPO nan'], 'midi out of range': ['TEMPO 120', 'KEYS 0 1 200 -10'],
             'empty file': [], 'bad note list': ['TEMPO 120', 'PAD 0 1 57,,60 -10'], 'missing value': ['TEMPO 120', 'SUB 0 1 33']}
    bad = []
    for n, sc in cases.items():
        R = run(sc, 'e')
        if R.rc == 0: bad.append(n + ' exits 0')
    R = run(['TEMPO 120', '# a comment', 'PAD 0 x 57 -10'], 'e'); line_ok = 'line 3' in R.err
    R2 = run(['TEMPO 120', 'WOBBLE 1'], 'e'); line_ok2 = 'line 2' in R2.err
    return not bad and line_ok and line_ok2, '; '.join(bad) or 'all exit non-zero, and name the line (3 and 2)'
check('errors exit non-zero and name the line', errors)
def peak_safe():
    R = run(['TEMPO 120', 'LEN 4', 'PAD 0 4 57,60,64 0', 'KEYS 0 4 60,64,67 0', 'SUB 0 4 33 0', 'REVERB 0'], 'ps')
    pk = db(np.abs(R.y).max())
    return R.rc == 0 and pk <= -0.9 and pk >= -1.2 and 'scaled by' in R.err, f'peak {pk:.2f} dBFS; ' + R.err.strip().split('\n')[0]
check('a hot sum is scaled to -1 dBFS and says so', peak_safe)
def length():
    R = run(['TEMPO 120', 'LEN 8', 'KEYS 0 1 60 -20'], 'ln')
    n = len(R.y) / SR
    return R.rc == 0 and abs(n - 8.0) < 0.01, f'{n:.3f} s (LEN 8 beats at 120 = 4 s, plus a 4 s tail)'
check('LEN plus a 4 s reverb tail sets the length', length)
def tail():
    R = run(['TEMPO 120', 'LEN 4', 'REVERB -6', 'KEYS 0 4 60 -10'], 'tl'); m = R.y.mean(axis=1)
    D0 = run(['TEMPO 120', 'LEN 4', DRY, 'KEYS 0 4 60 -10'], 'tl0'); m0 = D0.y.mean(axis=1)
    a = db(rms(seg(m, 2.5, 3.5))); a0 = db(rms(seg(m0, 2.5, 3.5))); late = db(rms(seg(m, 5.5, 5.9)))
    return R.rc == 0 and a > -65 and a0 < -90 and late < a - 20, f'tail {a:.1f} dBFS at 2.5-3.5 s (dry render {a0:.1f}), {late - a:.1f} dB from it at 5.5-5.9 s'
check('the reverb rings on after the last note', tail)
# ---- PIANO / PEDAL / SAMPLES (the real Salamander samples; skipped, not failed, when the directory is missing) ----
SAMP = '/Users/alex/Dev/soundtrack-samples/salamander/SalamanderGrandPianoV3_48khz24bit/48khz24bit'
HAVE_SAMP = os.path.isdir(SAMP) and os.path.exists(SAMP + '/A4v1.wav')
def piano(name, fn):
    if ONLY and ONLY not in name: return
    if not HAVE_SAMP: print('SKIP ' + name + ' - no sample directory ' + SAMP); sys.stdout.flush(); return
    check(name, fn)
def pk_hz(m, a, b, lo=100, hi=2000):
    f, P = spec(seg(m, a, b)); k = (f > lo) & (f < hi); return f[k][np.argmax(P[k])]
def piano_pitch():
    out = []; ok = True
    for midi, want in ((69, 440.0), (70, 466.1638)):
        R = run(['TEMPO 60', 'LEN 3', DRY, f'PIANO 0 3 {midi} 90'], 'pp'); m = R.y.mean(axis=1); h = pk_hz(m, 0.3, 2.3, 100, 1000)
        ok = ok and R.rc == 0 and abs(h - want) / want < 0.01; out.append(f'midi {midi}: {h:.1f} Hz (want {want:.1f})')
    return ok, '; '.join(out)
piano('PIANO at midi 69 is 440 Hz and at midi 70 (resampled) 466.2 Hz', piano_pitch)
def piano_vel():
    lv = {}
    for v in (30, 100, 104, 110):
        R = run(['TEMPO 60', 'LEN 3', DRY, f'PIANO 0 3 60 {v}'], 'pv'); lv[v] = db(rms(seg(R.y.mean(axis=1), 0.0, 1.0)))
    big = lv[110] - lv[30]; same = lv[104] - lv[100]     # 100 and 104 share layer 12: the gain alone, 30 * 4 / 127 = 0.94 dB
    return big >= 10 and 0.5 < same < 1.5 and big > 30 * 80 / 127 + 3, f'vel 110 is {big:.1f} dB above vel 30 (want >= 10, and more than the 18.9 dB of gain alone); vel 104 is {same:.2f} dB above vel 100 in the same layer (want 0.94)'
piano('a higher vel is louder (vel 110 at least 10 dB above vel 30)', piano_vel)
def piano_release():
    A = run(['TEMPO 60', 'LEN 3', DRY, 'PIANO 0 1 57 90'], 'pr1'); a = A.y.mean(axis=1)
    B = run(['TEMPO 60', 'LEN 4', DRY, 'PEDAL 0 4', 'PIANO 0 1 57 90'], 'pr2'); b = B.y.mean(axis=1)
    da = db(rms(seg(a, 0.45, 0.55))) - db(rms(seg(a, 1.75, 1.85))); dbb = db(rms(seg(b, 0.45, 0.55))) - db(rms(seg(b, 1.75, 1.85)))
    return A.rc == 0 and B.rc == 0 and da >= 30 and dbb < 12, f'dry: 1.8 s is {da:.1f} dB under 0.5 s (want >= 30); pedalled: {dbb:.1f} dB (want < 12)'
piano('a note releases after its duration (>= 30 dB at 1.8 s) and rings under PEDAL (< 12 dB)', piano_release)
def piano_pedal_end():
    R = run(['TEMPO 60', 'LEN 4', DRY, 'PEDAL 0 3', 'PIANO 0 1 57 90'], 'pe'); m = R.y.mean(axis=1)
    d = db(rms(seg(m, 2.85, 2.95))) - db(rms(seg(m, 3.75, 3.85)))
    return R.rc == 0 and d >= 30, f'0.8 s after the pedal span ends the note is {d:.1f} dB under its level just before (want >= 30)'
piano('a pedalled note is released when the span ends', piano_pedal_end)
def piano_chord():
    R = run(['TEMPO 60', 'LEN 3', DRY, 'PIANO 0 3 60,64,67 90'], 'pc'); f, P = spec(seg(R.y.mean(axis=1), 0.3, 2.3)); bad = []
    for midi in (60, 64, 67):
        w = 440 * 2 ** ((midi - 69) / 12); k = (f > w * 0.99) & (f < w * 1.01); near = (f > w * 0.9) & (f < w * 1.1)
        if P[k].max() < 10 ** 0.5 * np.median(P[near]) * 10: bad.append(midi)
    return R.rc == 0 and not bad, 'fundamentals present' if not bad else f'missing {bad}'
piano('a chord 60,64,67 shows all three fundamentals', piano_chord)
def piano_timing():
    R = run(['TEMPO 120', 'LEN 8', DRY, 'PIANO 4 1 69 90'], 'pt'); m = np.abs(R.y.mean(axis=1))
    on = int(np.argmax(m > 0.01 * m.max())) / SR
    return R.rc == 0 and abs(on - 2.0) < 0.008, f'starts at {on:.4f} s'
piano('a PIANO note at beat 4 of TEMPO 120 starts at 2.000 s', piano_timing)
def piano_det():
    sc = ['TEMPO 90', 'LEN 4', 'PEDAL 1 2', 'PIANO 0 2 60,64,67 70', 'PIANO 1.5 1 72.3 100', 'KEYS 1 2 72 -12']
    a = run(sc, 'pd1'); b = run(sc, 'pd2')
    return a.rc == 0 and a.bytes == b.bytes, f'same {a.bytes == b.bytes}'
piano('two PIANO renders are byte-identical', piano_det)
def piano_nodir():
    R = run(['TEMPO 60', 'SAMPLES /nonexistent', 'PIANO 0 1 60 90'], 'pn')
    return R.rc != 0 and '/nonexistent' in R.err and 'line 3' in R.err, f'rc {R.rc}, ' + R.err.strip()
check('SAMPLES /nonexistent is fatal and names the path and the line', piano_nodir)
# ---- INST / MANIFESTS / SAMPLEROOT ----
MAN = '/Users/alex/Dev/foolish-wt-city-mixes/foolish/tools/soundtrack/samples/manifests'
HAVE_INST = os.path.exists(MAN + '/lyria_haegeum.txt') and os.path.exists('/Users/alex/Dev/soundtrack-samples/lyria/haegeum/haegeum_A4_69.wav')
def inst_chk(name, fn):
    if ONLY and ONLY not in name: return
    if not HAVE_INST: print('SKIP ' + name + ' - no manifest/samples'); sys.stdout.flush(); return
    check(name, fn)
def inst_pitch():
    out = []; ok = True
    for midi, want in ((69, 440.0), (70, 466.1638)):
        R = run(['TEMPO 60', 'LEN 3', DRY, f'INST 0 2.5 {midi} 100 lyria_haegeum'], 'ip')
        m = R.y.mean(axis=1); h = pk_hz(m, 0.3, 2.0, 200, 1000)
        ok = ok and R.rc == 0 and abs(h - want) / want < 0.02; out.append(f'midi {midi}: {h:.1f} Hz (want {want:.1f})')
    return ok, '; '.join(out)
inst_chk('INST lyria_haegeum plays at 440 Hz (midi 69) and 466.2 Hz (midi 70 resampled)', inst_pitch)
def inst_vel():
    A = run(['TEMPO 60', 'LEN 2', DRY, 'INST 0 1.5 69 40 lyria_haegeum'], 'iv1')
    B = run(['TEMPO 60', 'LEN 2', DRY, 'INST 0 1.5 69 110 lyria_haegeum'], 'iv2')
    d = db(rms(seg(B.y.mean(axis=1), 0.2, 1.2))) - db(rms(seg(A.y.mean(axis=1), 0.2, 1.2)))
    return A.rc == 0 and B.rc == 0 and 12 < d < 20, f'vel 110 is {d:.1f} dB above vel 40 (want 16.5 dB)'
inst_chk('INST velocity scales dynamics cleanly', inst_vel)
def inst_loop():
    R = run(['TEMPO 60', 'LEN 8', DRY, 'INST 0 6 69 100 lyria_haegeum'], 'il'); m = R.y.mean(axis=1)
    mid = db(rms(seg(m, 0.5, 1.0))); late = db(rms(seg(m, 4.5, 5.5))); off = db(rms(seg(m, 6.6, 7.0)))
    return R.rc == 0 and mid - late < 12 and mid - off > 30, f'4.5-5.5 s is {mid - late:.1f} dB under 0.5-1.0 s (looping); 6.6-7.0 s is {mid - off:.1f} dB under (released)'
inst_chk('INST loops sustained samples past their length and releases after dur', inst_loop)
def inst_det():
    sc = ['TEMPO 90', 'LEN 4', 'INST 0 2 69,72 95 lyria_haegeum', 'INST 2 2 67 90 lyria_haegeum']
    a = run(sc, 'id1'); b = run(sc, 'id2')
    return a.rc == 0 and a.bytes == b.bytes, f'same {a.bytes == b.bytes}'
inst_chk('two INST renders are byte-identical', inst_det)
def inst_noman():
    R = run(['TEMPO 60', 'INST 0 1 60 90 nonexistent_manifest_xyz'], 'inm')
    return R.rc != 0 and 'nonexistent_manifest_xyz' in R.err and 'line 2' in R.err, f'rc {R.rc}, ' + R.err.strip()
check('INST with missing manifest is fatal and names the path and the line', inst_noman)
def brass_voice():
    R = run(['TEMPO 60', 'LEN 3', DRY, 'BRASS 0 2 57,60,64 -10'], 'br'); m = R.y.mean(axis=1)
    f, P = spec(seg(m, 0.2, 1.8)); k = (f > 180) & (f < 260); peak = f[k][np.argmax(P[k])]
    # filter envelope opens brighter at 0.12 s than at 1.2 s
    _, Pa = spec(seg(m, 0.08, 0.20)); _, Pb = spec(seg(m, 1.0, 1.2))
    fa = np.fft.rfftfreq(len(seg(m, 0.08, 0.20)), 1 / SR); fb = np.fft.rfftfreq(len(seg(m, 1.0, 1.2)), 1 / SR)
    hi_a = Pa[fa > 1000].sum() / (Pa.sum() + 1e-12); hi_b = Pb[fb > 1000].sum() / (Pb.sum() + 1e-12)
    return R.rc == 0 and abs(peak - 220) < 3.0 and hi_a > hi_b * 2.5, f'peak {peak:.1f} Hz, hi-ratio attack {hi_a:.3f} vs sustain {hi_b:.3f}'
check('BRASS plays at pitch with an opening brass filter envelope', brass_voice)
print('%d failed' % len(fails) if fails else 'all checks passed'); sys.exit(1 if fails else 0)
PY
T="$T" ONLY="${ONLY:-}" "$PY" -I "$T/t.py"
