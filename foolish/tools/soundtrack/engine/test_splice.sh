#!/bin/bash
# test_splice.sh: the city-mix renderer (splice.c) and mix_city.sh, on synthetic inputs only, self-cleaning.
# One PASS or FAIL line per check, non-zero exit if any check fails. Every check was mutation checked (the code it covers
# broken, the named check seen to fail, the code restored).
#   SPLICE_C=/path/to/copy.c  test a (mutated) copy of splice.c instead of the real one
#   ONLY=text                 run only the checks whose name contains the text
PY=/Users/alex/Dev/soundtrack-tools/venv/bin/python
D="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
SRC_C="${SPLICE_C:-$D/splice.c}"
clang -O2 -Wall -Werror -I"$D" -o "$T/splice" "$SRC_C" -framework AudioToolbox -framework CoreFoundation 2> "$T/cc.log" || { cat "$T/cc.log"; echo "FAIL build"; exit 1; }
[ -s "$T/cc.log" ] && { cat "$T/cc.log"; echo "FAIL build warnings"; exit 1; }
cat > "$T/t.py" <<'PY'
import numpy as np, subprocess, sys, os, re, math, scipy.signal as sg, scipy.io.wavfile as wf
SR = 48000
T = os.environ['T']; BIN = T + '/splice'; D = os.environ['D']; ONLY = os.environ.get('ONLY', '')
rng = np.random.default_rng(20261122)
fails = []

def W(name, x):
    x = np.asarray(x, dtype=np.float32)
    if x.ndim == 1: x = np.stack([x, x], 1)
    p = f'{T}/{name}.wav'; wf.write(p, SR, x); return p
def rd(p):
    r = subprocess.run(['ffmpeg', '-v', 'error', '-nostdin', '-i', p, '-f', 'f32le', '-ar', '48000', '-ac', '2', '-'], capture_output=True, check=True)
    return np.frombuffer(r.stdout, dtype='<f4').reshape(-1, 2).astype(np.float64)
def noise(sec, amp): return rng.standard_normal(int(sec * SR)) * amp
def pink(sec, amp):
    n = int(sec * SR); X = np.fft.rfft(rng.standard_normal(n)); f = np.arange(len(X)); f[0] = 1
    y = np.fft.irfft(X / np.sqrt(f), n); return y / np.std(y) * amp
def sine(sec, f, amp): t = np.arange(int(sec * SR)) / SR; return amp * np.sin(2 * np.pi * f * t)
def clicks(x, times, amp=0.9):
    x = np.array(x)
    for t in times: x[int(round(t * SR))] = amp
    return x
def db(x): return 20 * np.log10(abs(x) + 1e-12)
def rms_db(x): return db(np.sqrt(np.mean(np.square(x))))
def sg_(x, a, b): return x[int(round(a * SR)):int(round(b * SR))]
def peaks(x, thr, md=0.05):
    a = np.abs(x if x.ndim == 1 else x[:, 0]); idx = np.where(a > thr)[0]; out = []; i = 0
    while i < len(idx):
        j = i
        while j + 1 < len(idx) and idx[j + 1] - idx[i] < md * SR: j += 1
        g = idx[i:j + 1]; out.append(int(g[np.argmax(a[g])])); i = j + 1
    return out
class Res: pass
def run(lines, tag='t'):
    cp = f'{T}/{tag}.cue'; open(cp, 'w').write('\n'.join(lines) + '\n')
    o = f'{T}/{tag}.out.wav'; t = f'{T}/{tag}.tl'; s = f'{T}/{tag}.seams'
    for p in (o, t, s):
        if os.path.exists(p): os.remove(p)
    r = subprocess.run([BIN, cp, o, t, s], capture_output=True, text=True)
    R = Res(); R.rc = r.returncode; R.err = r.stderr; R.y = rd(o) if os.path.exists(o) else None; R.out = o
    def tab(p):
        if not os.path.exists(p): return []
        L = open(p).read().split('\n'); h = L[0].split('\t'); return [dict(zip(h, l.split('\t'))) for l in L[1:] if l]
    R.rows = tab(t); R.seams = tab(s); return R
def ebu(p):
    r = subprocess.run(['ffmpeg', '-nostats', '-nostdin', '-i', p, '-af', 'ebur128=peak=true', '-f', 'null', '-'], capture_output=True, text=True).stderr
    I = float(re.findall(r'^\s*I:\s+(-?[\d.]+) LUFS', r, re.M)[-1]); P = float(re.findall(r'^\s*Peak:\s+(-?[\d.]+) dBFS', r, re.M)[-1]); return I, P
def tone_amp(x, f, a, b):
    s = sg_(x, a, b); t = np.arange(len(s)) / SR
    return abs(2 * np.mean(s * np.exp(-2j * np.pi * f * t)))
def freq_zc(x, a, b):
    s = sg_(x, a, b); i = np.where((s[:-1] < 0) & (s[1:] >= 0))[0]
    fr = i + (-s[i]) / (s[i + 1] - s[i]); return (len(fr) - 1) * SR / (fr[-1] - fr[0])
def cents(f, f0): return 1200 * math.log2(f / f0)
def tfr(R):
    return [{k: (float(v) if re.match(r'^-?[\d.]+$', v) else v) for k, v in r.items()} for r in R.rows]
def check(name, fn):
    if ONLY and ONLY not in name: return
    try: ok, detail = fn()
    except Exception as e: ok, detail = False, 'exception %r' % (e,)
    print(('PASS ' if ok else 'FAIL ') + name + (' - ' + detail if detail else '')); sys.stdout.flush()
    if not ok: fails.append(name)
def s(t): return int(round(t * SR))

# ---------------------------------------------------------------- placement
bed = sine(10, 220, 0.03)
KP = W('k', clicks(bed, [1, 2, 3, 4, 5, 6, 7, 8, 9, 7.6, 5.45]))
def placement_clips():
    R = run(['LEVEL off', 'MASTER -36 -1.5', f'SRC k {KP}', 'CLIP k 1.5 3.5', 'CLIP k 5.5 7.5 GAP 0.25 PRE 0.1 POST 0.2'], 'pl1')
    ia2 = s(0.008) + s(2.0) + s(0.25)
    exp = [s(0.008) + s(2) - s(1.5), s(0.008) + s(3) - s(1.5), ia2 + s(5.45) - s(5.5), ia2 + s(6) - s(5.5), ia2 + s(7) - s(5.5), ia2 + s(7.6) - s(5.5)]
    got = peaks(R.y, 0.3)
    return R.rc == 0 and len(got) == len(exp) and all(abs(g - e) <= 1 for g, e in zip(got, exp)), f'{got} vs {exp}'
check('placement CLIP GAP PRE POST', placement_clips)
def placement_at():
    R = run(['LEVEL off', 'MASTER -36 -1.5', f'SRC k {KP}', 'CLIP k 1.5 3.5', 'CLIP k 5.5 7.5 GAP 0.25 PRE 0.1 POST 0.2', 'LAYER k 1.5 2.5 AT -0.5'], 'pl2')
    ia2 = s(0.008) + s(2.0) + s(0.25); b2 = ia2 + s(2.0); ia3 = b2 - s(0.5)
    exp = sorted([s(0.008) + s(2) - s(1.5), s(0.008) + s(3) - s(1.5), ia2 + s(5.45) - s(5.5), ia2 + s(6) - s(5.5), ia2 + s(7) - s(5.5), ia2 + s(7.6) - s(5.5), ia3 + s(2) - s(1.5)])
    got = peaks(R.y, 0.3, 0.03)
    return R.rc == 0 and len(got) == len(exp) and all(abs(g - e) <= 1 for g, e in zip(got, exp)), f'{got} vs {exp}'
check('placement LAYER AT', placement_at)
def placement_off():
    R = run(['LEVEL off', 'MASTER -36 -1.5', f'SRC k {KP}', 'CLIP k 1.5 3.5', 'LAYER k 3.5 4.5 OFF 0.3'], 'pl3')
    ia1 = s(0.008); ia3 = ia1 + s(0.3)
    exp = sorted([ia1 + s(2) - s(1.5), ia1 + s(3) - s(1.5), ia3 + s(4) - s(3.5)])
    got = peaks(R.y, 0.3, 0.03)
    return R.rc == 0 and len(got) == len(exp) and all(abs(g - e) <= 1 for g, e in zip(got, exp)), f'{got} vs {exp}'
check('placement LAYER OFF', placement_off)
def seams_cols():
    R = run(['LEVEL off', 'MASTER -36 -1.5', f'SRC k {KP}', 'CLIP k 1.5 3.5 ROLE big_hit', 'CLIP k 5.5 7.5 GAP 0.25 PRE 0.1 POST 0.2 ROLE next_one JOIN cross_fade', 'LAYER k 1.5 2.5 AT -0.5'], 'pl4')
    rows = R.rows; sm = R.seams
    ok = len(rows) == 3 and [r['idx'] for r in rows] == ['1', '2', '3'] and len(sm) == 1
    st = [float(r['song_start']) for r in rows]; ok = ok and st == sorted(st)
    ok = ok and rows[0]['role'] == 'big hit' and rows[1]['role'] == 'next one' and rows[1]['join_in'] == 'cross fade'
    ok = ok and rows[0]['src_path'] == KP and abs(float(rows[0]['src_in']) - 1.492) < 1e-3 and abs(float(rows[0]['src_out']) - 3.5) < 1e-3
    ok = ok and abs(float(rows[1]['src_in']) - 5.4) < 1e-3 and abs(float(rows[1]['src_out']) - 7.7) < 1e-3
    q = sm[0]
    ok = ok and q['idx'] == '2' and abs(float(q['time']) - 2.258) < 1e-5 and abs(float(q['prev_end']) - 2.008) < 1e-5 and abs(float(q['start']) - 2.158) < 1e-5 and q['join_in'] == 'cross fade'
    ok = ok and rows[0]['song_start'] == '0.000000' and set(R.rows[0].keys()) == {'idx', 'song_start', 'song_end', 'src_path', 'src_in', 'src_out', 'rate', 'semitones', 'gain_db', 'role', 'join_in'}
    return ok, str(sm) + str(rows[:2])
check('timeline and seams columns', seams_cols)

# ---------------------------------------------------------------- crossfade, level, master
NA = W('na', noise(12, 0.1)); NB = W('nb', noise(12, 0.1))
def xfade():
    R = run(['LEVEL off', f'SRC a {NA}', f'SRC b {NB}', 'CLIP a 1 6 PRE 0 FOUT 1', 'CLIP b 1 6 PRE 0 GAP -1 FIN 1'], 'xf')
    y = R.y[:, 0]; w = [rms_db(y[int(t * SR):int((t + 0.1) * SR)]) for t in np.arange(0.5, 8.4, 0.1)]
    ref = np.median(w); dev = max(abs(v - ref) for v in w)
    return R.rc == 0 and dev < 0.6, f'max deviation {dev:.2f} dB over the join'
check('equal-power crossfade', xfade)
LA = W('la', noise(12, 0.3)); LB = W('lb', noise(12, 0.02))
def levelling():
    R = run([f'SRC a {LA}', f'SRC b {LB}', 'CLIP a 1 7', 'CLIP b 1 7 GAP 0.5', 'CLIP a 1 7 GAP 0.5 LUFS -12'], 'lv')
    y = R.y[:, 0]; rows = tfr(R)
    lv = [rms_db(y[int((r['song_start'] + 0.5) * SR):int((r['song_end'] - 0.5) * SR)]) for r in rows]
    gd = rows[2]['gain_db'] - rows[0]['gain_db']
    return R.rc == 0 and abs(lv[0] - lv[1]) < 0.3 and abs(lv[2] - lv[0] - 4) < 0.3 and abs(gd - 4) < 0.05, f'levels {lv}, gain delta {gd:.3f}'
check('LUFS levelling', levelling)
CR = W('cr', clicks(noise(12, 0.05), [3, 6, 9], 0.8))
def master_loud():
    R = run([f'SRC c {CR}', 'CLIP c 1 9'], 'ms1'); I, P = ebu(R.out)
    dur = len(R.y) / SR
    ok1 = R.rc == 0 and abs(I + 16) <= 0.1 and P <= -1.5 + 1e-6 and abs(dur - 8.408) < 0.003
    R2 = run([f'SRC c {CR}', 'MASTER -20 -3', 'CLIP c 1 9'], 'ms2'); I2, P2 = ebu(R2.out)
    ok2 = R2.rc == 0 and abs(I2 + 20) <= 0.1 and P2 <= -3 + 1e-6
    return ok1 and ok2, f'{I:.3f} LUFS {P:.2f} dBTP dur {dur:.3f}; {I2:.3f} LUFS {P2:.2f} dBTP'
check('master loudness and ceiling', master_loud)

# ---------------------------------------------------------------- timeline truth, rate, pitch
def bumps(sec):
    t = np.arange(int(sec * SR)) / SR; env = np.full(len(t), 0.05)
    for tk, wk in zip(rng.uniform(0, sec, int(sec * 6)), rng.uniform(0.04, 0.12, int(sec * 6))): env += np.exp(-((t - tk) / wk) ** 2)
    return env
AMENV = bumps(60); AM = noise(60, 1) * AMENV * 0.2; AMP = W('am', AM)
def envk(x):
    a = np.abs(x); k = np.ones(960) / 960; return np.convolve(a, k, 'same')[::48]
def truth(extra, a, b, tag):
    R = run(['LEVEL off', f'SRC m {AMP}', f'CLIP m {a} {b} {extra}'], tag); r = tfr(R)[0]
    out = envk(R.y[:, 0]); srcenv = envk(AM); rate = r['rate']
    T0 = int((r['song_start'] + 0.7) * 1000); T1 = int((r['song_end'] - 0.7) * 1000)
    t = np.arange(T0, T1); ex = np.interp(r['src_in'] + (t / 1000 - r['song_start']) * rate, np.arange(len(srcenv)) / 1000, srcenv)
    res = {}
    for lag in range(-150, 151): res[lag] = np.corrcoef(out[t + lag], ex)[0, 1]
    lag = max(res, key=res.get)
    return R.rc == 0 and abs(lag) <= 5 and res[lag] > 0.9, f'lag {lag} ms corr {res[lag]:.3f}'
check('timeline truth RATE 1', lambda: truth('', 3, 14, 'tt1'))
check('timeline truth RATE 1.03', lambda: truth('RATE 1.03', 3, 18, 'tt2'))
check('timeline truth RATE 0.97', lambda: truth('RATE 0.97', 3, 18, 'tt3'))
check('timeline truth SEMI', lambda: truth('SEMI 4', 3, 14, 'tt4'))
check('timeline truth VARI', lambda: truth('RATE 1.05 VARI 1', 3, 18, 'tt5'))
FT = clicks(sine(34, 1000, 0.3), [2, 14, 28], 0.9); FTP = W('ft', FT)
def rate_case(extra, rate, semi, fexp):
    R = run(['LEVEL off', f'SRC f {FTP}', f'CLIP f 0 30 PRE 0 {extra}'], 'rt'); y = R.y[:, 0]; r = tfr(R)[0]
    f = freq_zc(y, 4, 12); dc = cents(f, fexp); bad = []
    for ct in (2, 14, 28):
        e = ct / rate; w = y[int((e - 0.15) * SR):int((e + 0.15) * SR)]; got = (np.argmax(np.abs(w)) - 0.15 * SR) / SR
        if abs(got) > 0.005: bad.append((ct, round(got * 1000, 1)))
    dur = r['song_end'] - r['song_start']
    return R.rc == 0 and abs(dc) <= 3 and not bad and abs(dur - 30 / rate) < 2e-5 and abs(r['semitones'] - semi) < 1e-3, f'{f:.3f} Hz ({dc:+.2f} cents), click errors ms {bad}, length {dur:.5f}'
check('RATE 1.03 frequency and impulse timing', lambda: rate_case('RATE 1.03', 1.03, 0, 1000))
check('RATE 0.97 frequency and impulse timing', lambda: rate_case('RATE 0.97', 0.97, 0, 1000))
check('SEMI +7 frequency and impulse timing', lambda: rate_case('SEMI 7', 1, 7, 1000 * 2 ** (7 / 12)))
check('VARI frequency and impulse timing', lambda: rate_case('RATE 1.1 VARI 1', 1.1, 12 * math.log2(1.1), 1100))

# ---------------------------------------------------------------- bands, EQ, ride
BN = noise(20, 0.1); BNP = W('bn', BN)
def transfer(R0, R1, fs):
    f, Pxy = sg.csd(R0.y[:, 0], R1.y[:, 0], SR, nperseg=8192); _, Pxx = sg.welch(R0.y[:, 0], SR, nperseg=8192)
    H = np.abs(Pxy / Pxx); return [20 * np.log10(np.mean(H[(f >= a) & (f <= b)])) for a, b in fs]
BREF = {}
def bref():
    if 'r' not in BREF: BREF['r'] = run(['LEVEL off', f'SRC n {BNP}', 'CLIP n 1 19 PRE 0'], 'bref')
    return BREF['r']
def bass_cut():
    R = run(['LEVEL off', f'SRC n {BNP}', 'CLIP n 1 19 PRE 0 BASS 0:-30'], 'b1'); lo, mid = transfer(bref(), R, [(30, 50), (500, 10000)])
    return R.rc == 0 and abs(lo + 30) < 2.5 and abs(mid) < 0.3, f'low {lo:.2f} dB, mid {mid:.2f} dB'
check('BASS -30 dB cuts the low band', bass_cut)
def bass_flat():
    R = run(['LEVEL off', f'SRC n {BNP}', 'CLIP n 1 19 PRE 0 BASS 0:0'], 'b0')
    bands = [(f * 2 ** (-1 / 6), f * 2 ** (1 / 6)) for f in (40, 63, 100, 150, 250, 400, 1000, 4000, 12000)]
    h = transfer(bref(), R, bands); w = max(abs(v) for v in h)
    return R.rc == 0 and w < 0.2, f'worst {w:.3f} dB'
check('BASS 0 dB is flat', bass_flat)
def eq_check():
    R = run(['LEVEL off', f'SRC n {BNP}', 'CLIP n 1 19 PRE 0 EQ 6:0:-12'], 'eq1'); lo, mid, hi = transfer(bref(), R, [(30, 50), (900, 1100), (10000, 14000)])
    R2 = run(['LEVEL off', f'SRC n {BNP}', 'CLIP n 1 19 PRE 0 EQ 0:8:0'], 'eq2'); a, b = transfer(bref(), R2, [(900, 1100), (150, 250)])
    return R.rc == 0 and abs((lo - mid) - 6) < 1.2 and abs((hi - mid) + 12) < 1.5 and abs((a - b) - 8) < 1.2, f'low {lo-mid:.2f} high {hi-mid:.2f} peak {a-b:.2f}'
check('EQ shelves and peak', eq_check)
def ride_check():
    R = run(['LEVEL off', f'SRC n {BNP}', 'CLIP n 2 14 PRE 0 RIDE 4:0,12:-12'], 'rd'); y = R.y[:, 0]
    l = lambda a, b: rms_db(sg_(y, a - 2, b - 2))
    d1 = l(8, 9) - l(2.5, 3.5); d2 = l(13, 14) - l(2.5, 3.5)
    return R.rc == 0 and abs(d1 + 6) < 0.8 and abs(d2 + 12) < 0.8, f'mid {d1:.2f} dB, end {d2:.2f} dB'
check('RIDE curve', ride_check)

# ---------------------------------------------------------------- filter sweeps
SW = sine(10, 300, 0.2) + sine(10, 12000, 0.1); SWP = W('sw', SW)
def d2max(x): return np.max(np.abs(x[2:] - 2 * x[1:-1] + x[:-2]))
SREF = {}
def sref():
    if 'r' not in SREF: SREF['r'] = run(['LEVEL off', f'SRC s {SWP}', 'CLIP s 1 9 PRE 0 FIN 0.05 FOUT 0.05'], 'sref')
    return SREF['r']
def sweep_case(key, hz, out, lowcomp, hicomp):
    R = run(['LEVEL off', f'SRC s {SWP}', f'CLIP s 1 9 PRE 0 FIN 0.05 FOUT 0.05 {key} 2:{hz}'], 'sw'); x = R.y[:, 0]; ref = sref().y[:, 0]
    n = min(len(x), len(ref)); x = x[:n]; ref = ref[:n]
    a, b = (0.4, 8 - 2 - 0.2) if out else (2 + 0.2, 7.6)
    xa = x[int(a * SR):int(b * SR)]; ra = ref[int(a * SR):int(b * SR)]; g = np.dot(xa, ra) / np.dot(ra, ra)
    ident = np.linalg.norm(xa - g * ra) / np.linalg.norm(xa)
    ca, cb = (7.5, 7.9) if out else (0.1, 0.4)
    def rel(sig): return db(tone_amp(sig, hicomp, ca, cb) / tone_amp(sig, lowcomp, ca, cb))
    drop = rel(x) - rel(ref)
    click = d2max(x) / (abs(g) * d2max(ref))
    return R.rc == 0 and ident < 1e-3 and drop < -20 and click < 1.2, f'outside residual {ident:.2e}, removed {drop:.1f} dB, click ratio {click:.2f}'
check('sweep LPOUT', lambda: sweep_case('LPOUT', 500, True, 300, 12000))
check('sweep HPOUT', lambda: sweep_case('HPOUT', 3000, True, 12000, 300))
check('sweep LPIN', lambda: sweep_case('LPIN', 500, False, 300, 12000))
check('sweep HPIN', lambda: sweep_case('HPIN', 3000, False, 12000, 300))

# ---------------------------------------------------------------- effects
def burst(sec, t0, f=1000, amp=0.4, width=0.04):
    x = np.zeros(int(sec * SR)); n = int(width * SR); w = np.hanning(n); i = int(t0 * SR) - n // 2
    x[i:i + n] = amp * w * np.sin(2 * np.pi * f * np.arange(n) / SR); return x
EB = W('eb', burst(6, 1.2))
def env_peak(y, t, half=0.03):
    h = np.abs(sg.hilbert(y)); a = int((t - half) * SR); w = h[a:int((t + half) * SR)]; return (a + int(np.argmax(w))) / SR, np.max(w)
def echo_data():
    R = run(['LEVEL off', 'MASTER -30 -1.5', f'SRC e {EB}', 'CLIP e 0.5 1.5 PRE 0 ECHO 0.3:0.5:-6:0.4'], 'ec'); return R, R.y[:, 0]
def echo_times():
    R, y = echo_data(); t0, a0 = env_peak(y, 0.7); bad = []
    for m in range(1, 10):
        tm, am = env_peak(y, 0.7 + 0.3 * m)
        if abs((tm - t0) - 0.3 * m) > 0.003: bad.append((m, round((tm - t0 - 0.3 * m) * 1000, 2)))
    n10 = db(env_peak(y, 0.7 + 3.0)[1] / a0)
    return R.rc == 0 and not bad and n10 < -57, f'time errors ms {bad}; repeat 10 at {n10:.1f} dB'
check('ECHO times', echo_times)
def echo_levels():
    R, y = echo_data(); t0, a0 = env_peak(y, 0.7); bad = []
    for m in range(1, 10):
        tm, am = env_peak(y, 0.7 + 0.3 * m); e = -6 - 6.0206 * (m - 1); g = db(am / a0)
        if abs(g - e) > 0.7: bad.append((m, round(g, 2), round(e, 2)))
    return R.rc == 0 and not bad, f'level errors {bad}'
check('ECHO levels', echo_levels)
PN = W('pn', pink(16, 0.1))
def wash_ring():
    R = run(['LEVEL off', f'SRC p {PN}', 'CLIP p 1 11 PRE 0 WASH 2:0:6'], 'w1'); y = R.y[:, 0]; end = 10.0
    dry = rms_db(sg_(y, end - 1, end - 0.2)); a = rms_db(sg_(y, end + 0.1, end + 0.5)) - dry; c = rms_db(sg_(y, end + 1.5, end + 2.0)) - dry
    tot = len(y) / SR - end
    return R.rc == 0 and -20 < a < 0 and c < a - 15 and 1.5 < tot < 1.3 * 2 + 0.5 + 0.41, f'ring {a:.1f} dB, later {c:.1f} dB, tail {tot:.2f} s'
check('WASH rings and decays', wash_ring)
def wash_level():
    R0 = run(['LEVEL off', f'SRC p {PN}', 'CLIP p 1 13 PRE 0'], 'w0'); R1 = run(['LEVEL off', f'SRC p {PN}', 'CLIP p 1 13 PRE 0 WASH 2:0:9'], 'w2')
    a, b = int(8 * SR), int(11 * SR); x = R1.y[a:b, 0]; r = R0.y[a:b, 0]; al = np.dot(x, r) / np.dot(r, r)
    wet = np.sqrt(np.mean((x - al * r) ** 2)); dry = al * np.sqrt(np.mean(r ** 2)); d = db(wet / dry)
    return R1.rc == 0 and abs(d) < 4, f'wet against dry {d:+.2f} dB'
check('WASH 0 dB is near the dry loudness', wash_level)
CK = W('ck', clicks(np.zeros(6 * SR), [2.0], 0.5)); NZ = W('nz', noise(8, 0.1))
def swell_case():
    R = run(['LEVEL off', 'MASTER -30 -1.5', f'SRC c {CK}', f'SRC n {NZ}', 'CLIP c 2.0 3.0 PRE 0 SWELL 1.5:0', 'CLIP n 1 5 GAP 1.5'], 'sl'); y = R.y[:, 0]; r = tfr(R)[0]
    ia = s(1.5)
    env = np.abs(y[:ia + s(0.3)]).copy(); env[ia - s(0.003):] = 0
    sm = np.convolve(env, np.ones(480) / 480, 'same'); pt = int(np.argmax(sm))
    early = rms_db(y[:s(0.05)]) - db(sm[pt]); rise = rms_db(y[s(1.2):s(1.45)]) - rms_db(y[s(0.2):s(0.45)]); after = rms_db(y[ia + s(0.01):ia + s(0.15)]) - db(sm[pt])
    return R.rc == 0 and abs(r['song_start'] - 1.5) < 1e-5 and 0.0 < (ia - pt) / SR < 0.2 and early < -25 and rise > 10 and after < -20, f'after a {after:.0f} dB, swell peak {(ia - pt) / SR * 1000:.0f} ms before a, start {early:.0f} dB, rise {rise:.1f} dB'
check('SWELL ends at a', swell_case)

# ---------------------------------------------------------------- brake, from, errors, mp3, many clips, mix_city
BR = W('br', sine(10, 1000, 0.3))
def brake_case():
    R = run(['LEVEL off', f'SRC b {BR}', 'CLIP b 0 4 PRE 0 BRAKE 2 FIN 0.01 FOUT 0.01'], 'bk'); y = R.y[:, 0]; r = tfr(R)[0]
    def fat(t): return freq_zc(y, t - 0.05, t + 0.05)
    f1, f2, f3 = fat(1.5), fat(2.5), fat(3.5); e = (1000, 750, 250)
    ok = all(abs(f - x) / x < 0.05 for f, x in zip((f1, f2, f3), e))
    return R.rc == 0 and ok and abs(r['src_out'] - 3.0) < 0.001 and abs(r['song_end'] - r['song_start'] - 4.0) < 2e-5, f'{f1:.0f} {f2:.0f} {f3:.0f} Hz, src_out {r["src_out"]:.4f}'
check('BRAKE slows to a stop', brake_case)
S1 = noise(10, 0.2); S2 = noise(10, 0.03)
def from_case():
    full = W('full', S1 + S2); st1 = W('st1', S1); st2 = W('st2', S2)
    R = run(['LEVEL -20', f'SRC f {full}', f'SRC s1 {st1} FROM {full}', f'SRC s2 {st2} FROM {full}', 'CLIP s1 2 8', 'LAYER s2 2 8 AT -6'], 'fr')
    R0 = run(['LEVEL -20', f'SRC f {full}', 'CLIP f 2 8'], 'fr0'); r = tfr(R); r0 = tfr(R0)[0]
    a = R.y[:, 0]; b = R0.y[:, 0]; n = min(len(a), len(b)); cc = np.corrcoef(a[:n], b[:n])[0, 1]
    ok = R.rc == 0 and r[0]['src_path'] == full and r[1]['src_path'] == full and abs(r[0]['gain_db'] - r[1]['gain_db']) < 1e-4 and abs(r[0]['gain_db'] - r0['gain_db']) < 0.02 and cc > 0.999
    return ok, f'gains {r[0]["gain_db"]:.3f} {r[1]["gain_db"]:.3f} full {r0["gain_db"]:.3f}, sum correlation {cc:.5f}'
check('FROM stems get the full-mix gain', from_case)
def bad_key():
    R = run([f'SRC n {NZ}', 'CLIP n 1 2 FOO 1'], 'bad'); return R.rc != 0 and 'line 2' in R.err and R.y is None, R.err.strip()
check('bad key fails', bad_key)
def bad_lines():
    cases = [(['BLAH x'], 2), (['CLIP n 1 zz'], 2), (['CLIP n 1 3', 'LAYER n 1 2 AT 1 OFF 1'], 3), (['CLIP n 3 4 PRE 5'], 2), (['CLIP n 1 7.99 POST 3'], 2),
             (['CLIP n 1 3', 'LAYER n 1 2 GAP 1'], 3), (['CLIP n 1 3 RATE'], 2), (['CLIP n 1 3 VARI 1'], 2), (['CLIP n 1 3 RIDE 1:2,0:3'], 2), (['CLIP n 1 3 EQ 1:2'], 2)]
    bad = []
    for body, ln in cases:
        R = run([f'SRC n {NZ}'] + body, 'bl')
        if R.rc == 0 or f'line {ln}' not in R.err or R.y is not None: bad.append((body, R.err.strip()))
    return not bad, str(bad)
check('fatal errors name the line', bad_lines)
def mp3_case():
    src = noise(6, 0.1); w = W('m3', src); mp = f'{T}/m3.mp3'
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', w, '-c:a', 'libmp3lame', '-b:a', '192k', mp], check=True)
    ref = rd(mp)[:, 0]; R = run(['LEVEL off', f'SRC m {mp}', 'CLIP m 1.0 4.0 PRE 0.5'], 'm3'); y = R.y[:, 0]
    seg = ref[s(1.0):s(3.0)]; best = max(range(-40, 41), key=lambda L: np.dot(y[s(0.5) + L:s(0.5) + L + len(seg)], seg))
    return R.rc == 0 and best == 0, f'best lag {best} samples'
check('decoded mp3 lands where ffmpeg puts it', mp3_case)
def many():
    h = [f'SRC s{i} {NZ}' for i in range(66)] + ['LEVEL off']
    for i in range(1300): h.append(f'CLIP s{i % 66} 0.5 0.55 GAP 0.01')
    R = run(h, 'many'); r = tfr(R)
    return R.rc == 0 and len(r) == 1300 and abs((r[-1]['song_start'] - r[0]['song_start']) - 1299 * 0.06) < 0.002, f'{len(r)} rows'
check('66 sources and 1300 clips', many)
def mixcity():
    root = f'{T}/city'; os.makedirs(root + '/cues'); open(root + '/cues/Test.cue.txt', 'w').write(f'SRC n {PN}\nCLIP n 1 13\n')
    mk = D + '/mix_city.sh'
    r = subprocess.run(['bash', mk, root + '/cues/Test.cue.txt'], capture_output=True, text=True)
    ok = r.returncode == 0 and len(r.stdout.strip().split('\n')) == 2 and all(os.path.exists(p) for p in (root + '/wav/Test.wav', root + '/cues/Test.timeline.tsv', root + '/work/Test.seams.tsv', root + '/work/Test.render.log', root + '/Test.m4a'))
    m1 = os.stat(root + '/work/bin/splice').st_mtime_ns
    r2 = subprocess.run(['bash', mk, root + '/cues/Test.cue.txt'], capture_output=True, text=True)
    ok = ok and r2.returncode == 0 and os.stat(root + '/work/bin/splice').st_mtime_ns == m1
    return ok, r.stdout.strip().replace('\n', ' | ') + r.stderr.strip()
check('mix_city.sh renders and verifies a root', mixcity)
def mixcity_bad():
    root = f'{T}/city2'; os.makedirs(root + '/cues'); open(root + '/cues/Bad.cue.txt', 'w').write(f'SRC n {PN}\nMASTER -20 -1.5\nCLIP n 1 13\n')
    r = subprocess.run(['bash', D + '/mix_city.sh', root + '/cues/Bad.cue.txt'], capture_output=True, text=True)
    return r.returncode != 0 and 'LUFS' in r.stderr, r.stderr.strip()
check('mix_city.sh fails off loudness', mixcity_bad)
def mixcity_len():
    ok = lambda v: subprocess.run(['bash', D + '/mix_city.sh', '--check-duration', v]).returncode == 0
    return ok('900.0') and ok('12.5') and not ok('900.001') and not ok('1800'), ''
check('mix_city.sh fails over 900 s', mixcity_len)
print('%d failed' % len(fails) if fails else 'all checks passed'); sys.exit(1 if fails else 0)
PY
T="$T" D="$D" ONLY="${ONLY:-}" "$PY" -I "$T/t.py"
