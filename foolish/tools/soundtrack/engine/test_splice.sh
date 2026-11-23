#!/bin/bash
# test_splice.sh: the city-mix renderer (splice.c) and mix_city.sh, on synthetic inputs only, self-cleaning.
# One PASS or FAIL line per check, non-zero exit if any check fails. Every check was mutation checked (the code it covers
# broken, the named check seen to fail, the code restored).
#   SPLICE_C=/path/to/copy.c  test a (mutated) copy of splice.c instead of the real one
#   PYTHON=/path/to/python  the python with numpy and scipy (default python3)
#   ONLY=text                 run only the checks whose name contains the text
PY="${PYTHON:-python3}"     # any python with numpy and scipy
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
def limiter_report():
    # a steady tone with one 0.2 s stretch 1.6 dB louder at 5 s of the source (4 s of the mix); the ceiling sits 0.2 dB over the steady peak,
    # so the limiter takes about 1.4 dB there and nothing elsewhere: the log must name that place (the suites allow 1 dB)
    x = sine(12, 440, 0.1); e = np.ones(len(x)); e[s(5.0):s(5.2)] = 10 ** (1.6 / 20); LR = W('lr', x * e)
    R = run(['LEVEL off', f'SRC c {LR}', 'MASTER -20 -19.1', 'CLIP c 1 9'], 'lim1')
    m = re.search(r'limiter: at most ([\d.]+) dB', R.err); ev = re.findall(r'over 1 dB at ([\d.]+) s: ([\d.]+) dB', R.err)
    ok = R.rc == 0 and m is not None and 1.0 < float(m.group(1)) < 2.0 and len(ev) == 1 and abs(float(ev[0][0]) - 4.1) < 0.15 and abs(float(ev[0][1]) - float(m.group(1))) < 0.01
    return ok, f'{m.group(0) if m else "no limiter line"}; events {ev}'
check('limiter events over 1 dB are listed', limiter_report)

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
    r0 = bref().y; n = min(len(r0), len(R.y)) - SR
    d = rms_db(R.y[:n] - r0[:n]) - rms_db(r0[:n])
    return R.rc == 0 and w < 0.2 and d < -70, f'worst {w:.3f} dB, against no BASS {d:.1f} dB'
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
SW = sine(10, 60, 0.2) + sine(10, 12000, 0.1); SWP = W('sw', SW)
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
    # no phase jump of the tone the filter shifts, where the sweep engages (OUT) or lets go (IN): the blend makes it a glide
    tone = hicomp; win = 48 if tone == 12000 else 480
    ra, rb = (5.9, 6.1) if out else (2.0, 2.3)
    z = sg.hilbert(x)[int(ra * SR):int(rb * SR)] * np.exp(-2j * np.pi * tone * (np.arange(int(ra * SR), int(rb * SR)) / SR))
    z = np.convolve(z, np.ones(win) / win, 'valid'); ph = np.unwrap(np.angle(z)); kick = np.max(np.abs(ph[win:] - ph[:-win]))
    return R.rc == 0 and ident < 1e-3 and drop < -20 and click < 1.2 and kick < 0.25, f'outside residual {ident:.2e}, removed {drop:.1f} dB, click ratio {click:.2f}, phase step {kick:.3f} rad'
check('sweep LPOUT', lambda: sweep_case('LPOUT', 500, True, 60, 12000))
check('sweep HPOUT', lambda: sweep_case('HPOUT', 3000, True, 12000, 60))
check('sweep LPIN', lambda: sweep_case('LPIN', 500, False, 60, 12000))
check('sweep HPIN', lambda: sweep_case('HPIN', 3000, False, 12000, 60))

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
def tails_stmt():
    def after(first, stmt='TAILS 2:0:1'):
        y = run(['LEVEL off', f'SRC p {PN}', stmt, f'{first} PRE 0', 'CLIP p 1 2 PRE 0 DRY 1 GAP 6'], 'ts').y[:, 0]
        return rms_db(sg_(y, 5.1, 5.6))
    wet, dry, off = after('CLIP p 1 6'), after('CLIP p 1 6 DRY 1'), after('CLIP p 1 6', 'TAILS 2:0:1\nTAILS off')
    own = after('CLIP p 1 6 WASH 2:-30:1')
    ok = wet > -60 and dry < -80 and off < -80 and own < wet - 20
    lay = run(['LEVEL off', f'SRC p {PN}', 'TAILS 2:0:1', 'CLIP p 1 2 PRE 0 DRY 1', 'LAYER p 1 2 AT 0 PRE 0', 'CLIP p 1 2 PRE 0 DRY 1 GAP 5'], 'tl').y[:, 0]
    la = rms_db(sg_(lay, 2.1, 2.6))
    return ok and la < -80, f'tail {wet:.1f} dB, DRY {dry:.1f}, off {off:.1f}, own WASH {own:.1f}, layer {la:.1f}'
check('TAILS rings under every cut unless DRY', tails_stmt)
def trimmed_end():
    q = np.concatenate([pink(4, 0.1), np.zeros(3 * SR)]); src = W('silent_end', q)
    R = run(['LEVEL off', f'SRC q {src}', 'CLIP q 0.5 7 PRE 0'], 'te'); n = len(R.y) / SR; r = R.rows[0]
    e, si, so = float(r['song_end']), float(r['src_in']), float(r['src_out'])
    return R.rc == 0 and e <= n + 1e-6 and abs((so - si) - (e - float(r['song_start']))) < 1e-3, f'file {n:.3f} s, row ends {e:.3f}, source span {so - si:.3f}'
check('a row ends where the trimmed file ends', trimmed_end)
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
# ---------------------------------------------------------------- FX one-shots and the BUS filter
ZP = W('zero', np.zeros(14 * SR)); LOWP = W('low', sine(14, 100, 0.05))
def fxrun(fx, src=ZP, tag='fx', clip='CLIP z 1 11 PRE 0', extra=()):
    return run(['LEVEL off', 'MASTER -30 -1.5', f'SRC z {src}', clip] + list(extra) + fx, tag)
def band(x, f, kind):
    return sg.sosfiltfilt(sg.butter(6, f, kind, fs=SR, output='sos'), x)
def brms(x, a, b): return rms_db(sg_(x, a, b))
def centroid(x, a, b):
    seg = sg_(x, a, b) * np.hanning(int(round((b - a) * SR))); P = np.abs(np.fft.rfft(seg)) ** 2; f = np.fft.rfftfreq(len(seg), 1 / SR)
    return float(np.sum(f * P) / np.sum(P))
def fx_crash():
    R = fxrun(['FX CRASH OFF 2 LEN 3 DB -20'], tag='fxc'); h = band(R.y[:, 0], 3000, 'hp')
    after = brms(h, 2.0, 2.2); before = brms(h, 1.8, 2.0); late = brms(h, 4.3, 4.5); low = brms(band(R.y[:, 0], 1000, 'lp'), 2.0, 2.2) - after
    return R.rc == 0 and after - before > 40 and late - after < -30 and low < -30, f'after {after:.1f} dB, before {before:.1f} dB, at 0.8 LEN {late - after:.1f} dB under the start, under 1 kHz {low:.1f} dB under'
check('FX CRASH starts at the anchor and decays', fx_crash)
def fx_revcym():
    R = fxrun(['FX REVCYM OFF 2 LEN 3 DB -20'], src=LOWP, tag='fxr'); h = band(R.y[:, 0], 3000, 'hp')
    last = brms(h, 4.8, 5.0); first = brms(h, 2.0, 2.6); gone = brms(h, 5.05, 5.35)
    return R.rc == 0 and first - last < -30 and gone - last < -60, f'first 20% {first - last:.1f} dB, after the end {gone - last:.1f} dB, last 200 ms {last:.1f} dB'
check('FX REVCYM swells to its end', fx_revcym)
def sweep_case(kind, up):
    R = fxrun([f'FX {kind} OFF 2 LEN 4 DB -20'], tag='fxs'); y = R.y[:, 0]
    c0 = centroid(y, 2.0, 3.0); c1 = centroid(y, 5.0, 6.0); l0 = brms(y, 2.0, 2.4); l1 = brms(y, 5.5, 5.9); tail = brms(y, 6.05, 6.35)
    ok = (c1 > 4 * c0 and l1 - l0 > 15) if up else (c0 > 4 * c1 and l0 - l1 > 15)
    return R.rc == 0 and ok and tail < min(l0, l1) - 40, f'centroid {c0:.0f} -> {c1:.0f} Hz, level {l0:.1f} -> {l1:.1f} dB, after the end {tail:.1f}'
check('FX RISER sweeps up and swells', lambda: sweep_case('RISER', True))
check('FX DOWN sweeps down and fades', lambda: sweep_case('DOWN', False))
def fx_boom():
    R = fxrun(['FX BOOM OFF 2 LEN 2 DB -20'], tag='fxb'); y = R.y[:, 0]; lo = band(y, 80, 'lp'); hi = band(y, 2000, 'hp')
    after = brms(lo, 2.05, 2.25); before = brms(lo, 1.5, 1.7); top = brms(hi, 2.05, 3.9)
    return R.rc == 0 and after - before > 60 and top - after < -60 and np.max(np.abs(R.y[:, 0] - R.y[:, 1])) < 1e-6, f'sub {after:.1f} dB over {before:.1f}, above 2 kHz {top - after:.1f} dB under'
check('FX BOOM is a mono sub impact', fx_boom)
def onsets(x, a, b):
    e = np.convolve(np.abs(band(x, 1500, 'hp')), np.ones(48) / 48, 'same'); out = []; i = int(a * SR)
    while i < int(b * SR):
        prev = np.max(e[i - 480:i - 144]) if i > 480 else 0
        if e[i] > 3 * prev + 1e-5 and e[i] > 2e-4: out.append(i); i += int(0.03 * SR)
        else: i += 1
    return out
def fx_roll():
    R = fxrun(['FX ROLL OFF 1 LEN 4 BEAT 1 DB -20'], tag='fxo'); y = R.y[:, 0]
    first = onsets(y, 0.99, 2.99); mid = onsets(y, 2.99, 3.99); last = onsets(y, 3.99, 5.05); allh = onsets(y, 0.99, 5.05)
    ratio = (len(last) / 1.0) / (len(first) / 2.0)
    pk = lambda i: np.max(np.abs(band(y, 1500, 'hp')[i:i + int(0.03 * SR)]))
    rise = db(pk(allh[-1]) / pk(allh[0])) if allh else 0
    return R.rc == 0 and (len(first), len(mid), len(last)) == (4, 4, 8) and abs(ratio - 4) < 0.5 and rise > 12 and abs(allh[-1] / SR - 4.875) < 0.01, f'hits {len(first)} {len(mid)} {len(last)}, density ratio {ratio:.2f}, last over first {rise:.1f} dB, last at {allh[-1] / SR if allh else 0:.3f}'
check('FX ROLL builds on its grid', fx_roll)
def bus_case(typ, curve, band_f, kind):
    cue = ['LEVEL off', 'MASTER -22 -1.5', f'SRC n {NA}', 'CLIP n 1 11 PRE 0']
    R0 = run(cue, 'bus0'); R = run(cue + [f'BUS {typ} OFF 2 CURVE {curve}'], 'bus1'); y = R.y[:, 0]
    g0 = 10 ** (tfr(R0)[0]['gain_db'] / 20); g1 = 10 ** (tfr(R)[0]['gain_db'] / 20)
    x = band(y, band_f, kind); mid = brms(x, 3.2, 3.8); pre = brms(x, 1.0, 1.8); post = brms(x, 5.5, 6.5)
    n = min(len(R0.y), len(R.y)); d = np.abs(R.y[:n, 0] / g1 - R0.y[:n, 0] / g0)
    out = np.concatenate([d[:s(1.99)], d[s(5.01):s(9.9)]])
    return R.rc == 0 and pre - mid > 60 and post - mid > 60 and np.max(out) < 1e-4 and np.max(d[s(3.2):s(3.8)]) > 0.01, f'in the region {mid:.1f} dB, before {pre:.1f}, after {post:.1f}, outside differs by {np.max(out):.2e}'
check('BUS LP closes the highs and leaves the rest', lambda: bus_case('LP', '0:20000,1:400,2:400,3:20000', 2000, 'hp'))
check('BUS HP closes the lows and leaves the rest', lambda: bus_case('HP', '0:20,1:3000,2:3000,3:20', 500, 'lp'))
def fx_all():
    fx = ['FX CRASH OFF 1 LEN 2 DB -20', 'FX REVCYM OFF 2 LEN 2 DB -22', 'FX RISER OFF 3 LEN 2 DB -24', 'FX DOWN OFF 4 LEN 2 DB -24',
          'FX BOOM OFF 5 LEN 2 DB -20', 'FX ROLL OFF 6 LEN 2 BEAT 0.5 DB -22']
    a = fxrun(fx, src=LOWP, tag='fd1', extra=['BUS LP OFF 1 CURVE 0:20000,1:800,2:20000']); b = fxrun(fx, src=LOWP, tag='fd2', extra=['BUS LP OFF 1 CURVE 0:20000,1:800,2:20000'])
    ok = a.rc == 0 and open(a.out, 'rb').read() == open(b.out, 'rb').read() and len(a.y) > 7 * SR
    return ok, 'two renders byte-identical'
check('FX and BUS render bit-identically', fx_all)
def fx_level():
    bad = []
    for k in ('CRASH', 'REVCYM', 'RISER', 'DOWN', 'BOOM', 'ROLL'):
        R = fxrun([f'FX {k} OFF 1 LEN 3 DB -20' + (' BEAT 0.5' if k == 'ROLL' else '')], tag='fl'); r = tfr(R)[0]; y = R.y
        kw = lambda x: sg.lfilter([1, -2, 1], [1, -1.99004745483398, 0.99007225036621], sg.lfilter([1.53512485958697, -2.69169618940638, 1.19839281085285], [1, -1.69065929318241, 0.73248077421585], x))
        p = kw(y[:, 0]) ** 2 + kw(y[:, 1]) ** 2; cs = np.concatenate([[0], np.cumsum(p)]); w = s(0.4); st = np.arange(0, len(p) - w, s(0.01))
        loud = -0.691 + 10 * np.log10(np.max(cs[st + w] - cs[st]) / w) - r['gain_db']
        if abs(loud + 20) > 1 or R.rc != 0: bad.append((k, round(loud, 2)))
    return not bad, f'off by more than 1 dB: {bad}'
check('FX DB is the loudest 400 ms window', fx_level)
def fx_anchor():
    def onset(line):
        R = fxrun([line], tag='fa'); y = np.abs(R.y[:, 0]); return R.rc, float(np.argmax(y > 0.05 * np.max(y))) / SR
    a = onset('FX CRASH AT -2 LEN 1 DB -20'); o = onset('FX CRASH OFF 1 LEN 1 DB -20')
    return a[0] == 0 and o[0] == 0 and abs(a[1] - 8.0) < 0.01 and abs(o[1] - 1.0) < 0.01, f'AT -2 at {a[1]:.4f} s, OFF 1 at {o[1]:.4f} s'
check('FX AT and OFF anchors', fx_anchor)
def fx_bus_anchor():
    cue = ['LEVEL off', 'MASTER -22 -1.5', f'SRC n {NA}', 'CLIP n 1 11 PRE 0']
    a = run(cue + ['BUS LP AT -8 CURVE 0:20000,1:400,2:400,3:20000'], 'ba1'); o = run(cue + ['BUS LP OFF 2 CURVE 0:20000,1:400,2:400,3:20000'], 'ba2')
    n = min(len(a.y), len(o.y)); ga = 10 ** (tfr(a)[0]['gain_db'] / 20); go = 10 ** (tfr(o)[0]['gain_db'] / 20)
    return a.rc == 0 and o.rc == 0 and np.max(np.abs(a.y[:n] / ga - o.y[:n] / go)) < 1e-4, 'BUS AT -8 equals BUS OFF 2 on a 10 s clip'
check('BUS AT and OFF anchors', fx_bus_anchor)
def fx_errors():
    ok = 'CURVE 0:20000,1:400,2:20000'
    cases = [(['FX WHOOSH OFF 1 LEN 2 DB -20'], 5), (['FX CRASH OFF 1 DB -20'], 5), (['FX ROLL OFF 1 LEN 2 DB -20'], 5), (['FX CRASH OFF 1 LEN 2'], 5),
             (['FX CRASH AT 1 OFF 1 LEN 2 DB -20'], 5), (['FX CRASH LEN 2 DB -20'], 5), (['FX CRASH OFF 1 LEN zz DB -20'], 5), (['FX CRASH OFF 1 LEN 2 DB -20 FOO 1'], 5),
             (['FX CRASH OFF 1 LEN 2 DB -20 BEAT 1'], 5),
             (['BUS LP OFF 1 CURVE 0:4000,1:400,2:20000'], 5), (['BUS LP OFF 1 CURVE 0:20000,1:400,2:4000'], 5), (['BUS HP OFF 1 CURVE 0:100,1:3000,2:20'], 5),
             (['BUS LP OFF 1 ' + ok, 'BUS LP OFF 1.5 ' + ok], 6), (['BUS LP OFF 1 CURVE 0:20000'], 5), (['BUS LP OFF 1 AT 1 ' + ok], 5), (['BUS LP OFF 1'], 5),
             (['BUS XX OFF 1 ' + ok], 5), (['BUS LP OFF 1 CURVE 1:20000,0:400,2:20000'], 5)]
    bad = []
    for body, ln in cases:
        R = run(['LEVEL off', f'SRC z {ZP}', 'CLIP z 1 11 PRE 0'] + [''] + body, 'fe')
        if R.rc == 0 or f'line {ln}' not in R.err or R.y is not None: bad.append((body, R.err.strip()))
    R = run(['LEVEL off', f'SRC z {ZP}', 'FX CRASH OFF 1 LEN 2 DB -20', 'CLIP z 1 11'], 'fe2')
    if R.rc == 0 or 'line 3' not in R.err: bad.append(('FX before CLIP', R.err.strip()))
    return not bad, str(bad)
check('FX and BUS errors name the line', fx_errors)
def fx_not_in_tables():
    R = fxrun(['FX CRASH OFF 2 LEN 2 DB -20', 'FX ROLL OFF 3 LEN 2 BEAT 0.5 DB -20', 'BUS LP OFF 1 CURVE 0:20000,1:400,2:20000'], tag='ft', extra=['CLIP z 1 5 PRE 0 GAP 0.5'])
    txt = open(f'{T}/ft.tl').read() + open(f'{T}/ft.seams').read()
    return R.rc == 0 and len(R.rows) == 2 and len(R.seams) == 1 and 'CRASH' not in txt and 'ROLL' not in txt and 'FX' not in txt and R.err.count('FX ') == 2 and 'BUS LP' in R.err, f'{len(R.rows)} rows, {len(R.seams)} seam, log lines FX {R.err.count("FX ")}'
check('FX and BUS stay out of the timeline and seams', fx_not_in_tables)

def bus_fast():
    cue = ['LEVEL off', 'MASTER -22 -1.5', f'SRC n {NA}', 'CLIP n 1 11 PRE 0']
    R = run(cue + ['BUS LP OFF 2 CURVE 0:20000,0.04:300,1:300,1.04:20000'], 'bf'); x = band(R.y[:, 0], 2000, 'hp')
    quick = brms(x, 2.06, 2.10); ref = brms(x, 1.0, 1.8)
    return R.rc == 0 and ref - quick > 40, f'band above 2 kHz at 60-100 ms into a 40 ms close: {quick - ref:.1f} dB against the dry'
check('BUS follows a fast curve', bus_fast)
def fx_fades():
    def ends(kind, at_end):
        R = fxrun([f'FX {kind} OFF 2 LEN 2 DB -20'], tag='ff'); y = R.y[:, 0]; e = s(4.0) if at_end else s(2.0); h = s(0.0005)
        a, b = (y[e - h:e], y[e - s(0.02):e - s(0.01)]) if at_end else (y[e:e + h], y[e + s(0.01):e + s(0.02)])
        return float(np.sqrt(np.mean(a ** 2)) / np.sqrt(np.mean(b ** 2)))
    r = {'RISER end': ends('RISER', True), 'REVCYM end': ends('REVCYM', True), 'DOWN start': ends('DOWN', False)}
    return all(v < 0.2 for v in r.values()), ' '.join(f'{k} {v:.3f}' for k, v in r.items()) + ' (last or first 0.5 ms against 10-20 ms in)'
check('FX start and end are faded', fx_fades)
print('%d failed' % len(fails) if fails else 'all checks passed'); sys.exit(1 if fails else 0)
PY
T="$T" D="$D" ONLY="${ONLY:-}" "$PY" -I "$T/t.py"
