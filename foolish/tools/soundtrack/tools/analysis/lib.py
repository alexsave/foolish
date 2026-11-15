# lib.py - shared DSP for the audio analysis tools (numpy and scipy only; ffmpeg for decoding and loudness).
#
# What it does: decode audio, STFT, onset envelope, tempo (windowed autocorrelation, Viterbi tempo map, global
#   tempo), beat grid, chroma and key, chords per bar, bass and melody salience, HPSS, sections by novelty,
#   band shares, loudness (BS.1770 K-weighting, ffmpeg ebur128), ending class, a voice heuristic.
# Inputs: audio file paths (anything ffmpeg decodes) or a mono 44.1 kHz float32 array (run_core mono_override).
# Outputs: Python values only; the report and file writing live in analyze_clip.py and measure_render.py.
# How to run: it is a library; the scripts add this folder to sys.path (python3 -I does not) and import it.
#   Example: python3 -I test_analysis.py   (exercises it end to end on synthetic signals).
# Known failure modes: tempo can lock to a half or double level (octave error) on dense or sparse material;
#   the key is one global key per file (modulations and relative major/minor confuse it); ffmpeg missing or an
#   undecodable file gives an empty signal and a crash further on.
# Randomness: the ONLY random source is rng() below; test_analysis.py greps the folder to enforce it.
import numpy as np, subprocess, re, os
from scipy import signal, ndimage

SR = 22050
NOTE = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B']
MAJ = np.array([6.35,2.23,3.48,2.33,4.38,4.09,2.52,5.19,2.39,3.66,2.29,2.88])
MIN = np.array([6.33,2.68,3.52,5.38,2.60,3.53,2.54,4.75,3.98,2.69,3.34,3.17])
KEYNAME_MAJ = ['C','Db','D','Eb','E','F','F#','G','Ab','A','Bb','B']
KEYNAME_MIN = ['C','C#','D','Eb','E','F','F#','G','G#','A','Bb','B']
HOP0 = 128   # onset-envelope hop at 22050 Hz (172.27 Hz)
HOP_BEAT_ENV = 256
SEED = 20261114   # the one named seed; change this line to fix every random draw of the tools to another constant

def rng(seed=SEED):
    """The single seedable random source of this folder (used by the synthetic test signals only)."""
    return np.random.default_rng(seed)  # RNG-SOURCE

# ---------------------------------------------------------------- io
def decode44(path):
    raw = subprocess.run(['ffmpeg','-v','error','-i',path,'-f','f32le','-ac','2','-ar','44100','-'],
                         capture_output=True).stdout
    return np.frombuffer(raw, dtype=np.float32).reshape(-1, 2)

def ebur128(path):
    r = subprocess.run(['ffmpeg','-nostats','-i',path,'-af','ebur128=peak=true','-f','null','-'],
                       capture_output=True, text=True).stderr
    s = r[r.rfind('Summary:'):]
    def g(pat):
        m = re.search(pat, s)
        return float(m.group(1)) if m else float('nan')
    return g(r'I:\s+(-?[\d.]+) LUFS'), g(r'LRA:\s+(-?[\d.]+) LU'), g(r'Peak:\s+(-?[\d.]+) dBFS')

def to22(mono44):
    return signal.resample_poly(mono44, 1, 2).astype(np.float32)

# ---------------------------------------------------------------- stft
def stft_mag(x, n, hop, maxhz=None, sr=SR):
    x = np.pad(x, (n//2, n//2), mode='reflect')
    nfr = 1 + (len(x) - n)//hop
    win = np.hanning(n).astype(np.float32)
    nb = n//2 + 1 if maxhz is None else int(maxhz/(sr/n)) + 1
    out = np.empty((nfr, nb), np.float32)
    idx0 = np.arange(n)
    for i in range(0, nfr, 2048):
        j = min(nfr, i+2048)
        fr = np.stack([x[k*hop:k*hop+n] for k in range(i, j)]) * win
        out[i:j] = np.abs(np.fft.rfft(fr, axis=1))[:, :nb]
    return out

def mel_fb(n, sr, nm, fmin, fmax):
    f = np.linspace(0, sr/2, n//2+1)
    mel = lambda h: 2595*np.log10(1+h/700.)
    imel = lambda m: 700*(10**(m/2595.)-1)
    pts = imel(np.linspace(mel(fmin), mel(fmax), nm+2))
    fb = np.zeros((nm, len(f)), np.float32)
    for i in range(nm):
        l, c, r = pts[i], pts[i+1], pts[i+2]
        up = (f-l)/(c-l); dn = (r-f)/(r-c)
        fb[i] = np.clip(np.minimum(up, dn), 0, None)
    fb /= (fb.sum(1, keepdims=True)+1e-12)
    return fb

# ---------------------------------------------------------------- loudness (BS.1770 K weighting at any fs)
def kweight(x, fs):
    G, Q, fc = 3.999843853973347, 0.7071752369554196, 1681.9744509555319
    K = np.tan(np.pi*fc/fs); Vh = 10**(G/20); Vb = Vh**0.4996667741545416
    a0 = 1+K/Q+K*K
    b = [(Vh+Vb*K/Q+K*K)/a0, 2*(K*K-Vh)/a0, (Vh-Vb*K/Q+K*K)/a0]
    a = [1, 2*(K*K-1)/a0, (1-K/Q+K*K)/a0]
    y = signal.lfilter(b, a, x)
    Q2, fc2 = 0.5003270373238773, 38.13547087613982
    K = np.tan(np.pi*fc2/fs); a0 = 1+K/Q2+K*K
    y = signal.lfilter([1, -2, 1], [1, 2*(K*K-1)/a0, (1-K/Q2+K*K)/a0], y)
    return y.astype(np.float64)

class Loud:
    def __init__(self, x, fs):
        y = kweight(x, fs)
        self.cs = np.concatenate([[0], np.cumsum(y*y)])
        self.fs = fs; self.n = len(x)
    def ms(self, t0, t1):
        i0 = int(max(0, min(self.n, round(t0*self.fs)))); i1 = int(max(i0+1, min(self.n, round(t1*self.fs))))
        return (self.cs[i1]-self.cs[i0])/max(1, i1-i0)
    def lufs(self, t0, t1):
        return -0.691 + 10*np.log10(self.ms(t0, t1)+1e-12)
    def curve(self, win=3.0, hop=0.5):
        dur = self.n/self.fs
        t = np.arange(0, dur+1e-9, hop)
        return t, np.array([self.lufs(max(0, x-win), x) if x > 0.2 else -120 for x in t])

# ---------------------------------------------------------------- tempo
def acf_norm(seg):
    seg = seg - seg.mean()
    n = len(seg); nf = 1 << (2*n-1).bit_length()
    F = np.fft.rfft(seg, nf)
    a = np.fft.irfft(F*np.conj(F))[:n]
    return a/(a[0]+1e-12)

def acf_peaks(a, fs, lo=50., hi=210., prior_c=120., prior_sig=1.3, maxn=5):
    """a: normalized autocorrelation of the onset envelope; fs: frames per second of that envelope (lag to BPM)."""
    fse = fs
    n = len(a)
    lmin = max(2, int(np.floor(60*fse/hi))); lmax = min(n//2-2, int(np.ceil(60*fse/lo)))
    if lmax <= lmin+2: return []
    L = np.arange(lmin, lmax+1)
    ea = a[L].copy()
    bpm = 60*fse/L
    wt = np.exp(-0.5*(np.log2(bpm/prior_c)/prior_sig)**2)
    sc = ea*wt
    pk, _ = signal.find_peaks(sc)
    out = []
    for p in pk:
        l = L[p]
        if 1 <= p < len(L)-1:
            y0, y1, y2 = ea[p-1], ea[p], ea[p+1]
            den = y0-2*y1+y2
            d = 0.5*(y0-y2)/den if den != 0 else 0.
            d = max(-1, min(1, d))
        else:
            d = 0.
        lf = l+d
        out.append((60*fse/lf, sc[p], float(a[int(round(lf))])))
    out.sort(key=lambda r: -r[1])
    return out[:maxn]

def tempo_windows(env, fs, win=8.0, hop=4.0):
    n = len(env); w = int(min(win*fs, n)); h = int(hop*fs)
    starts = list(range(0, max(1, n-w+1), h))
    res = []; acfs = []
    for s in starts:
        a = acf_norm(env[s:s+w])
        pk = acf_peaks(a, fs)
        acfs.append(a)
        t0 = s/fs
        if pk: res.append((t0, t0+w/fs, pk[0][0], pk[0][2], pk))
        else: res.append((t0, t0+w/fs, float('nan'), 0., []))
    return res, acfs

def viterbi_tempo(wins, lam=0.5):
    """Pick one candidate per window so that score is high and tempo changes between neighbouring windows are
    penalised by lam per octave.  Returns wins with (t0,t1,bpm,strength,peaks,raw_bpm)."""
    n = len(wins)
    cands = [w[4] if w[4] else [(float('nan'), 0., 0.)] for w in wins]
    cost = [np.array([-c[1] for c in cs]) for cs in cands]
    back = []
    acc = cost[0].copy()
    for i in range(1, n):
        new = np.empty(len(cands[i])); bk = np.empty(len(cands[i]), int)
        for j, cj in enumerate(cands[i]):
            tr = acc.copy()
            for k, ck in enumerate(cands[i-1]):
                if np.isnan(cj[0]) or np.isnan(ck[0]): tr[k] += 0.5
                else: tr[k] += lam*abs(np.log2(cj[0]/ck[0]))
            k = int(np.argmin(tr)); new[j] = tr[k]+cost[i][j]; bk[j] = k
        back.append(bk); acc = new
    path = [int(np.argmin(acc))]
    for bk in back[::-1]: path.append(int(bk[path[-1]]))
    path = path[::-1]
    out = []
    for w, cs, p in zip(wins, cands, path):
        out.append((w[0], w[1], cs[p][0], cs[p][2], w[4], w[2]))
    return out

def fold_to(w, ref, tol=0.08):
    best = w
    for k in (-1, 1):
        c = w*2.0**k
        if abs(np.log(c/ref)) < abs(np.log(best/ref)): best = c
    return best

def fine_refine(env, fs, b0, span=0.04, step=0.0005, nh=4):
    e = env - env.mean()
    t = np.arange(len(e))/fs
    grid = b0*(1+np.arange(-span, span+1e-9, step))
    sc = np.zeros(len(grid))
    for i, b in enumerate(grid):
        for k in range(1, nh+1):
            sc[i] += abs(np.sum(e*np.exp(-2j*np.pi*k*b*t/60.)))/k**0.5
    sc = ndimage.gaussian_filter1d(sc, 1)
    return grid[int(np.argmax(sc))]

def global_tempo(env, fs, wins, acfs):
    mean_acf = np.mean(acfs, axis=0)
    pk = acf_peaks(mean_acf, fs)
    if not pk: return float('nan'), float('nan'), 0., 0., False, float('nan'), False
    best = pk[0][0]
    second = float('nan')
    for p in pk[1:]:
        if abs(p[0]/best-1) > 0.06: second = p[0]; break
    strength = pk[0][2]
    vals = np.array([w[2] for w in wins if not np.isnan(w[2])])
    folded = np.array([fold_to(v, best) for v in vals])
    std = float(np.std(folded)) if len(folded) else float('nan')
    stable = std/best < 0.02
    if stable:
        best = fine_refine(env, fs, best)
    elif len(folded):
        best = float(np.median(folded))
    oct_ratio = float('nan'); doubled = False
    if 2*best <= 200:
        oct_ratio = level_support(env, fs, best)
        if oct_ratio >= 0.85:
            second = best; best = 2*best; doubled = True
    return best, second, strength, std, stable, oct_ratio, doubled

def level_support(env, fs, slow_bpm):
    """odd/even test for a x2 tempo ambiguity: ratio of onset strength halfway between slow beats to on the slow beats."""
    P = 60*fs/slow_bpm
    n = len(env)
    pk = ndimage.maximum_filter1d(env, 5)
    best = None
    for ph in np.arange(0, P, 1.0):
        idx = (ph+np.arange(0, (n-P)/P)*P).astype(int)
        v = pk[idx].mean()
        if best is None or v > best[0]: best = (v, ph)
    ph = best[1]
    k = np.arange(0, int((n-P-ph)/P))
    even = pk[(ph+k*P).astype(int)].mean(); odd = pk[(ph+(k+0.5)*P).astype(int)].mean()
    return odd/(even+1e-12)

# ---------------------------------------------------------------- beats
def beat_track(envb, fs, bpm_t, alpha=100.):
    n = len(envb)
    e = ndimage.gaussian_filter1d(envb/(envb.std()+1e-9), 1.0)
    P = 60*fs/bpm_t
    C = np.zeros(n); B = -np.ones(n, int)
    for t in range(n):
        p = P[t]
        lo = int(max(0, t-round(2*p))); hi = int(t-round(p/2))
        if hi <= lo:
            C[t] = e[t]; continue
        tau = np.arange(lo, hi+1)
        sc = C[lo:hi+1]-alpha*np.log((t-tau)/p)**2
        j = int(np.argmax(sc))
        if sc[j] > 0:
            C[t] = e[t]+sc[j]; B[t] = lo+j
        else:
            C[t] = e[t]
    p_end = int(P[-1])
    t = int(n-p_end+np.argmax(C[max(0, n-p_end):])) if n > p_end else int(np.argmax(C))
    beats = []
    while t >= 0:
        beats.append(t); t = B[t]
    return np.array(beats[::-1])

def bar_phase(beat_strength, ms=(3, 4)):
    n = len(beat_strength)
    b = (beat_strength-beat_strength.mean())/(beat_strength.std()+1e-9)
    expmax = {3: 0.85, 4: 1.03, 2: 0.56, 6: 1.27}
    res = {}
    for m in ms:
        if n < 3*m: continue
        prof = np.array([b[p::m].mean() for p in range(m)])
        ph = int(np.argmax(prof))
        z = (prof.max()-prof.mean())*np.sqrt(n/m) - expmax[m]
        res[m] = (z, ph)
    if not res: return 4, 0, 0., 0.
    m = 4 if 4 in res else max(res, key=lambda k: res[k][0])
    if 3 in res and 4 in res and res[3][0] > res[4][0]+1.0: m = 3
    other = [res[k][0] for k in res if k != m]
    return m, res[m][1], res[m][0], (res[m][0]-max(other) if other else float('nan'))

# ---------------------------------------------------------------- chroma / key
def est_tuning(S4, df):
    k = np.arange(1, S4.shape[1]-1)
    sel = S4[::3]
    num = 0j; wsum = 0.
    lo = int(100/df); hi = int(2000/df)
    for fr in sel:
        seg = fr[lo:hi]
        if seg.max() <= 0: continue
        pk, _ = signal.find_peaks(seg, height=0.1*seg.max())
        pk = pk[(pk > 0) & (pk < len(seg)-1)] + lo
        if len(pk) == 0: continue
        a = np.log(fr[pk-1]+1e-9); b = np.log(fr[pk]+1e-9); c = np.log(fr[pk+1]+1e-9)
        d = 0.5*(a-c)/(a-2*b+c-1e-12)
        f = (pk+np.clip(d, -.5, .5))*df
        dev = 12*np.log2(f/440.)
        w = fr[pk]
        num += np.sum(w*np.exp(2j*np.pi*dev)); wsum += w.sum()
    if wsum == 0: return 0., 0.
    R = abs(num)/wsum
    cents = np.angle(num)/(2*np.pi)*100
    return (float(cents) if R > 0.2 else 0.), float(R)

def chroma_from(S4, df, tuning_cents, fmin=80., fmax=3500.):
    nb = S4.shape[1]
    f = np.arange(nb)*df
    ok = (f >= fmin) & (f <= fmax)
    midi = 69+12*np.log2(np.maximum(f, 1e-3)/440.)-tuning_cents/100.
    pc = np.mod(np.round(midi).astype(int), 12)
    M = np.zeros((nb, 12), np.float32)
    M[np.arange(nb)[ok], pc[ok]] = 1
    C = np.sqrt(S4) @ M
    return C/(np.linalg.norm(C, axis=1, keepdims=True)+1e-9)

def keyname(i, minor): return (KEYNAME_MIN if minor else KEYNAME_MAJ)[i] + (' minor' if minor else ' major')

def key_est(chroma):
    v = chroma.mean(0)
    sc = []
    for mode, prof in ((0, MAJ), (1, MIN)):
        for r in range(12):
            sc.append((np.corrcoef(v, np.roll(prof, r))[0, 1], r, mode))
    sc.sort(key=lambda t: -t[0])
    return keyname(sc[0][1], sc[0][2]), sc[0][0]-sc[1][0], keyname(sc[1][1], sc[1][2]), sc[0], sc[1]

TEMPL = []
for r in range(12):
    for mi in (0, 1):
        t = np.zeros(12); t[r] = 1; t[(r+(3 if mi else 4)) % 12] = 1; t[(r+7) % 12] = 1
        TEMPL.append((r, mi, t/np.linalg.norm(t)))
def chord_of(vec):
    n = np.linalg.norm(vec)
    if n < 1e-6: return 'N', 0., 0.
    v = vec/n
    sc = np.array([t @ v for _, _, t in TEMPL])
    o = np.argsort(-sc)
    r, mi, _ = TEMPL[o[0]]
    return NOTE[r]+('m' if mi else ''), float(sc[o[0]]), float(sc[o[0]]-sc[o[1]])
def chord_label(vec, margin=0.03, minscore=0.55):
    c, s, m = chord_of(vec)
    if c == 'N' or m < margin or s < minscore: return 'unclear', c, s, m
    return c, c, s, m

# ---------------------------------------------------------------- pitch salience
def salience(S, df, cand_f, nh=5, decay=0.8, fmax=6000.):
    """S: T x F magnitude (any compression applied by caller). returns T x len(cand_f)."""
    T, F = S.shape
    out = np.zeros((T, len(cand_f)), np.float32)
    for h in range(1, nh+1):
        p = cand_f*h/df
        ok = (cand_f*h < fmax) & (p < F-2)
        i0 = np.floor(p[ok]).astype(int); fr = (p[ok]-i0).astype(np.float32)
        v = S[:, i0]*(1-fr)+S[:, i0+1]*fr
        out[:, ok] += (decay**(h-1))*v
    return out

def midi_name(m):
    m = int(round(m)); return NOTE[m % 12]+str(m//12-1)

def hpss_crop(S, th=17, tp=17):
    H = ndimage.median_filter(S, size=(th, 1), mode='nearest')
    P = ndimage.median_filter(S, size=(1, tp), mode='nearest')
    mk = H**2/(H**2+P**2+1e-12)
    return S*mk, S*(1-mk)

def whiten(S, k=31):
    env = ndimage.uniform_filter1d(S, k, axis=1, mode='nearest')
    return np.minimum(S/(env+1e-6), 20.)

# ================================================================ analysis core (from analyze.py)
BANDS = [('sub', 0, 60), ('bass', 60, 250), ('lowmid', 250, 1000), ('mid', 1000, 4000), ('high', 4000, 10000), ('air', 10000, 1e9)]

def ending(mono44, env, fs):
    n100 = 4410
    N = len(mono44)//n100
    fr = mono44[len(mono44)-N*n100:].reshape(N, n100)
    lv = 10*np.log10((fr.astype(np.float64)**2).mean(1)+1e-12)
    last1 = 10*np.log10((mono44[-44100:].astype(np.float64)**2).mean()+1e-12)
    if N >= 60: ref = np.percentile(lv[-60:-20], 75)
    else: ref = np.percentile(lv[:max(1, N-5)], 75)
    endl = lv[-1]
    drop = ref-endl
    if endl < -70 and lv[-3:].max() < -70: pass
    thr = ref-6
    idx = np.where(lv >= thr)[0]
    tail = (N-1-idx[-1])*0.1 if len(idx) else 0.
    if drop < 12 or tail < 0.5:
        kind = 'hard cut'
    else:
        ne = int(2*fs)
        tail_on = env[-ne:].max() if len(env) > ne else env.max()
        refon = np.percentile(env[-int(8*fs):-int(2.5*fs)], 90) if len(env) > 10*fs else np.percentile(env, 90)
        kind = 'ring-out' if tail_on < 0.35*refon else 'fade'
    return last1, kind, drop, tail

def checker_novelty(F, L):
    n = len(F)
    Z = (F-F.mean(0))/(F.std(0)+1e-6)
    Z /= (np.linalg.norm(Z, axis=1, keepdims=True)+1e-9)
    S = Z @ Z.T
    g = np.exp(-0.5*(np.arange(-L, L)+0.5)**2/(L/2.)**2)
    G = np.outer(g, g)
    sg = np.sign(np.arange(-L, L)+0.5)
    K = G*np.outer(sg, sg)
    P = np.pad(S, L, mode='constant')
    nov = np.zeros(n)
    for i in range(n):
        nov[i] = np.sum(K*P[i:i+2*L, i:i+2*L])
    return np.maximum(nov, 0)

def run_core(path, mono_override=None):
    r = {}
    if mono_override is None:
        st = decode44(path); mono44 = st.mean(1).astype(np.float32)
    else:
        mono44 = mono_override; st = np.stack([mono44, mono44], 1)
    dur = len(mono44)/44100.
    r['dur'] = dur
    x = to22(mono44)
    r['x'] = x; r['mono44'] = mono44
    if mono_override is None:
        r['lufs'], r['lra'], r['tp'] = ebur128(path)
    else:
        r['lufs'] = r['lra'] = r['tp'] = float('nan')
    M = (st[:, 0]+st[:, 1])/2; Sd = (st[:, 0]-st[:, 1])/2
    r['sm_db'] = 10*np.log10(((Sd.astype(np.float64)**2).sum())/((M.astype(np.float64)**2).sum()+1e-12)+1e-12)
    # 44.1k band frames
    S44 = stft_mag(mono44, 2048, 1024, sr=44100)
    P44 = S44.astype(np.float64)**2
    f44 = np.arange(P44.shape[1])*(44100/2048.)
    bandE = np.stack([P44[:, (f44 >= lo) & (f44 < hi)].sum(1) for _, lo, hi in BANDS], 1)
    r['bandE'] = bandE
    r['centroid'] = float((P44 @ f44).sum()/(P44.sum()+1e-12))
    del S44, P44
    # S1 / mel hpss
    S1 = stft_mag(x, 2048, 512)
    fb = mel_fb(2048, SR, 128, 30, 11000)
    mel = S1 @ fb.T
    del S1
    Hm = ndimage.median_filter(mel, size=(31, 1), mode='nearest')
    Pm = ndimage.median_filter(mel, size=(1, 13), mode='nearest')
    mk = Hm**2/(Hm**2+Pm**2+1e-12)
    te = (mel**2).sum(1); pe = ((mel*(1-mk))**2).sum(1)
    r['te'] = te; r['pe'] = pe; r['perc_share'] = float(pe.sum()/(te.sum()+1e-12))
    del mel, Hm, Pm, mk
    # onset envelope
    S0 = stft_mag(x, 1024, HOP0)
    fb40 = mel_fb(1024, SR, 40, 30, 10000)
    m40 = S0 @ fb40.T
    del S0
    lm = 20*np.log10(m40+1e-6); lm = np.maximum(lm, lm.max()-80)
    lag = 4
    fl = np.maximum(0, lm[lag:]-lm[:-lag])
    env = np.concatenate([np.zeros(lag), fl.mean(1)])
    envlow = np.concatenate([np.zeros(lag), fl[:, :8].mean(1)])
    fs = SR/HOP0
    r['env'] = env; r['fs'] = fs
    wins, acfs = tempo_windows(env, fs)
    wins = viterbi_tempo(wins)
    best, second, strength, std, stable, octr, dbl = global_tempo(env, fs, wins, acfs)
    r.update(bpm=best, bpm2=second, pulse=strength, tstd=std, tstable=stable, wins=wins, octr=octr, doubled=dbl)
    # onset density
    es = ndimage.gaussian_filter1d(env, 1.5)
    pk, _ = signal.find_peaks(es, prominence=0.5*es.std(), distance=int(0.05*fs))
    r['onsets'] = pk/fs; r['onset_dens'] = len(pk)/dur
    # tempo track + beats
    valid = [(0.5*(w[0]+w[1]), w[2]) for w in wins if not np.isnan(w[2])]
    if not valid or np.isnan(best):
        r['beats'] = np.array([]); return r
    prev = best; tr = []
    for c, w in valid:
        q = w/prev
        if abs(q/2-1) < 0.08: w /= 2
        elif abs(q*2-1) < 0.08: w *= 2
        tr.append(w); prev = w
    tr = np.array(tr)
    mm = np.median(tr)
    for k in (2, 0.5):
        if abs(mm/best/k-1) < 0.1: tr = tr/k
    if len(tr) >= 3: tr = ndimage.median_filter(tr, 3, mode='nearest')
    cen = np.array([v[0] for v in valid])
    r['track'] = (cen, tr)
    fb_ = fs/2
    nb = len(env)//2
    envb = env[:2*nb].reshape(nb, 2).max(1); lowb = envlow[:2*nb].reshape(nb, 2).max(1)
    tb = np.arange(nb)/fb_
    bt = np.interp(tb, cen, tr)
    zs = lambda v: (v-v.mean())/(v.std()+1e-9)
    beats_f = beat_track(zs(envb)+0.7*zs(lowb), fb_, bt)
    beats = beats_f/fb_
    r['beats'] = beats
    zs = lambda v: (v-v.mean())/(v.std()+1e-9)
    ev = ndimage.maximum_filter1d(zs(envb), 3)[beats_f]+ndimage.maximum_filter1d(zs(lowb), 3)[beats_f]
    m, ph, z, marg = bar_phase(ev)
    r.update(m=m, phase=ph, mz=z, mmarg=marg, bstr=ev)
    bar_idx = np.arange(ph, len(beats), m)
    r['bars'] = beats[bar_idx]
    sb = ev[bar_idx]
    good = bar_idx[sb >= np.median(sb)]
    r['first_down'] = float(beats[good[0]]) if len(good) else (float(beats[ph]) if len(beats) > ph else float('nan'))
    # level support if ratio 2 vs prompt is decided by caller
    # chroma / key
    S4 = stft_mag(x, 4096, 512, maxhz=6000)
    df = SR/4096.
    cents, R = est_tuning(S4, df)
    r['tuning'] = cents; r['tuneR'] = R
    ch = chroma_from(S4, df, cents)
    w = np.sqrt(S4.sum(1)); w = w/(w.sum()+1e-9)
    chw = (ch*w[:, None]).sum(0, keepdims=True)
    kname, kconf, k2, k1t, k2t = key_est(ch*w[:, None]*len(w))
    r.update(key=kname, kconf=kconf, key2=k2, S4=S4, chroma=ch)
    r['end'] = ending(mono44, env, fs)
    return r

def sections(r):
    ch = r['chroma']; bandE = r['bandE']
    T = min(len(ch), len(bandE))
    fps = 44100/1024.
    g = (np.arange(T)/fps/0.5).astype(int)
    ng = g.max()+1
    cnt = np.bincount(g, minlength=ng).astype(float)
    def avg(A):
        out = np.stack([np.bincount(g, weights=A[:T, j], minlength=ng) for j in range(A.shape[1])], 1)
        return out/cnt[:, None]
    chg = avg(ch)
    bd = avg(10*np.log10(bandE[:T]+1e-9))
    F = np.concatenate([(chg-chg.mean(0))/(chg.std(0)+1e-6)/np.sqrt(12), (bd-bd.mean(0))/(bd.std(0)+1e-6)/np.sqrt(6)], 1)
    dur = r['dur']
    L = 12 if dur > 40 else 6
    nov = checker_novelty(F, L)
    mind = int(6/0.5) if dur > 40 else int(3/0.5)
    thr = nov.mean()+0.5*nov.std()
    pk, _ = signal.find_peaks(nov, height=thr, distance=mind)
    t = (pk+0.5)*0.5
    t = t[(t > 2.5) & (t < dur-2.5)]
    r['nov'] = nov; r['nov_t'] = (np.arange(len(nov)))*0.5
    return [0.]+list(t)+[dur]

def stats(r, loud, t0, t1):
    fps = 44100/1024.
    i0 = int(t0*fps); i1 = max(i0+1, int(t1*fps))
    be = r['bandE'][i0:i1].sum(0)
    shares = 10*np.log10(be/(be.sum()+1e-12)+1e-12)
    on = ((r['onsets'] >= t0) & (r['onsets'] < t1)).sum()/max(1e-6, t1-t0)
    j0 = int(t0*SR/512); j1 = max(j0+1, int(t1*SR/512))
    ps = r['pe'][j0:j1].sum()/(r['te'][j0:j1].sum()+1e-12)
    tempo = float('nan')
    if 'track' in r:
        cen, tr = r['track']
        sel = (cen >= t0) & (cen < t1)
        tempo = float(np.median(tr[sel])) if sel.any() else float(np.interp(0.5*(t0+t1), cen, tr))
    return dict(lufs=loud.lufs(t0, t1), shares=shares, onset=on, perc=ps, tempo=tempo)

def spec_image(mono44, nc=1600, nrow=256):
    n = 4096; hop_min = 512
    dur = len(mono44)
    nc = int(min(nc, max(50, (dur-n)//hop_min)))
    cen = np.linspace(n//2, dur-n//2-1, nc).astype(int)
    win = np.hanning(n).astype(np.float32)
    img = np.empty((nc, n//2+1), np.float32)
    for i, c in enumerate(cen):
        img[i] = np.abs(np.fft.rfft(mono44[c-n//2:c+n//2]*win))
    P = img**2
    P = ndimage.uniform_filter1d(P, 3, axis=1)
    fq = np.logspace(np.log10(30), np.log10(16000), nrow)
    fb = np.arange(P.shape[1])*(44100/n)
    out = np.empty((nc, nrow), np.float32)
    for i in range(nc):
        out[i] = np.interp(fq, fb, P[i])
    db = 10*np.log10(out+1e-10)
    return cen/44100., fq, db

# ------------------------------------------------------------ per bar
def bar_table(r, a, b, tuning):
    beats = r['beats']; bars = r['bars']; m = r['m']
    ch = r['chroma']; S4 = r['S4']; x = r['x']
    fps = SR/512.
    sel = [i for i in range(len(bars)) if a-1e-6 <= bars[i] < b]
    if not sel: return ['(no bars found in this window)']
    # crop for melody
    ia = int(max(0, a-0.5)*fps); ib = min(len(S4), int((b+0.5)*fps)+1)
    crop = S4[ia:ib]
    Hh, _ = hpss_crop(crop)
    Hh = np.sqrt(Hh); Hh = whiten(Hh)
    df = SR/4096.
    midis = np.arange(55, 100)
    candf = 440*2.0**((midis-69)/12.+tuning/1200.)
    sal = salience(Hh, df, candf, nh=5, decay=0.8)
    en = np.sqrt((crop[:, int(200/df):int(2500/df)]**2).sum(1))
    gate = 0.08*np.percentile(en, 90)
    # bass
    xs = x[int(max(0, a-1)*SR):int(min(len(x), (b+1)*SR))]
    xb = signal.resample_poly(xs, 1, 4)
    Sb = stft_mag(xb, 4096, 256, maxhz=400, sr=SR/4.)
    dfb = (SR/4.)/4096
    bm = np.arange(28, 56)
    bf = 440*2.0**((bm-69)/12.+tuning/1200.)
    bsal = salience(np.sqrt(Sb), dfb, bf, nh=4, decay=0.85, fmax=400)
    bfps = (SR/4.)/256
    boff = max(0, a-1)
    rows = []
    for k in sel:
        t0 = bars[k]; t1 = bars[k+1] if k+1 < len(bars) else (beats[-1] if beats[-1] > t0 else t0+1)
        bb = beats[(beats >= t0-1e-6) & (beats < t1-1e-6)]
        if len(bb) < 2: continue
        # chords
        i0, i1 = int(t0*fps), max(int(t0*fps)+1, int(t1*fps))
        c, cg, sc, mg = chord_label(ch[i0:i1].mean(0))
        cs = c if c != 'unclear' else 'unclear (%s?)' % cg
        half = ''
        if m == 4 and len(bb) == 4:
            tm = bb[2]
            j = int(tm*fps)
            c1, g1, s1, m1 = chord_label(ch[i0:j].mean(0)) if j > i0 else ('unclear',)*4
            c2, g2, s2, m2 = chord_label(ch[j:i1].mean(0)) if i1 > j else ('unclear',)*4
            if c1 != 'unclear' and c2 != 'unclear' and c1 != c2: half = ' (%s|%s)' % (c1, c2)
        # bass
        ibeats = list(bb)+[t1]
        bseq = []; btot = np.zeros(len(bm))
        for q in range(len(ibeats)-1):
            j0 = int((ibeats[q]-boff)*bfps); j1 = max(j0+1, int((ibeats[q+1]-boff)*bfps))
            v = bsal[j0:j1].mean(0) if j1 <= len(bsal) and j0 < len(bsal) else np.zeros(len(bm))
            btot += v
            bseq.append(v)
        bmax = bsal.max()+1e-9
        def bn(v):
            if v.max() < 0.05*bmax: return '-'
            return midi_name(bm[int(np.argmax(v))])
        bass = bn(btot/len(bseq)) if True else ''
        bseqs = ' '.join(bn(v) for v in bseq)
        # melody per half beat
        mel = []; mg_l = []
        pts = []
        for q in range(len(ibeats)-1):
            pts += [ibeats[q], 0.5*(ibeats[q]+ibeats[q+1])]
        pts.append(t1)
        for q in range(len(pts)-1):
            j0 = int((pts[q])*fps)-ia; j1 = max(j0+1, int(pts[q+1]*fps)-ia)
            j0 = max(0, j0); j1 = min(len(sal), max(j0+1, j1))
            if j0 >= len(sal) or en[j0:j1].mean() < gate: mel.append('-'); continue
            v = sal[j0:j1].mean(0)
            i = int(np.argmax(v)); s1 = v[i]
            far = np.abs(np.arange(len(v))-i) >= 2
            s2 = v[far].max() if far.any() else 0
            mgn = (s1-s2)/(s1+1e-9)
            mg_l.append(mgn)
            mel.append(midi_name(midis[i])+('?' if mgn < 0.1 else ''))
        rows.append((t0, cs+half, '%.2f / %.2f' % (sc, mg), bass, bseqs, ' '.join(mel), np.mean(mg_l) if mg_l else float('nan')))
    out = ['| bar | start (s) | chord | chord score / margin | bass (bar) | bass per beat | melody per half beat (octave guess) | mean salience margin |', '|---|---|---|---|---|---|---|---|']
    for i, (t0, cs, mg, bass, bseqs, mel, mm) in enumerate(rows):
        out.append('| %d | %.2f | %s | %s | %s | %s | %s | %.2f |' % (i+1, t0, cs, mg, bass, bseqs, mel, mm))
    return out

# ------------------------------------------------------------ voice heuristic
def voice_heuristic(r, tuning):
    S4 = r['S4']; df = SR/4096.; fps = SR/512.
    T = len(S4)
    b0, b1 = int(300/df), int(3400/df)
    band = S4[:, b0:b1]
    tot = S4.sum(1)**1.0
    e_ratio = (band**2).sum(1)/((S4**2).sum(1)+1e-9)
    # harmonic share within band
    Hh = ndimage.median_filter(band, size=(17, 1), mode='nearest')
    Pp = ndimage.median_filter(band, size=(1, 17), mode='nearest')
    hmask = Hh**2/(Hh**2+Pp**2+1e-12)
    h_share = ((band*hmask)**2).sum(1)/((band**2).sum(1)+1e-9)
    # f0 track 150-1000 Hz, 25 cent grid
    midis = np.arange(55, 84, 0.25)
    candf = 440*2.0**((midis-69)/12.+tuning/1200.)
    sub = np.sqrt(S4)
    sub = whiten(sub)
    sal = salience(sub, df, candf, nh=4, decay=0.8)
    best = sal.argmax(1); sv = sal.max(1)
    f0c = midis[best]*100.
    conf = sv/(np.median(sal, axis=1)+1e-9)
    voiced = conf > 3.0
    nw = int(2*fps)
    res = []
    for w0 in range(0, T-nw+1, nw):
        sl = slice(w0, w0+nw)
        s_e = float(np.clip((e_ratio[sl].mean()-0.35)/0.35, 0, 1))
        s_h = float(np.clip((h_share[sl].mean()-0.5)/0.4, 0, 1))
        # vibrato
        vib = 0.; 
        v = voiced[sl]; c = f0c[sl]
        i = 0
        while i < nw:
            if not v[i]: i += 1; continue
            j = i
            while j+1 < nw and v[j+1] and abs(c[j+1]-c[j]) < 150: j += 1
            if j-i+1 >= 22:
                seg = c[i:j+1]
                seg = seg-ndimage.uniform_filter1d(seg, 21, mode='nearest')
                if len(seg) >= 22:
                    rms = seg.std()
                    sp = np.abs(np.fft.rfft(seg*np.hanning(len(seg)), 256))**2
                    fr = np.fft.rfftfreq(256, 1/fps)
                    ratio = sp[(fr >= 4) & (fr <= 8)].sum()/(sp[(fr >= 1) & (fr <= 12)].sum()+1e-9)
                    if 15 <= rms <= 150: vib = max(vib, ratio)
            i = j+1
        s_v = float(np.clip((vib-0.3)/0.4, 0, 1))
        # formant-like broad peaks
        ms = np.log(band[sl].mean(0)+1e-9)*20/np.log(10)
        sm = ndimage.gaussian_filter1d(ms, 120/df)
        pk, pr = signal.find_peaks(sm, prominence=2.5, distance=int(400/df))
        s_f = float(np.clip((len(pk)-1)/2., 0, 1))
        score = 0.2*s_e+0.3*s_h+0.3*s_v+0.2*s_f
        res.append((w0/fps, (w0+nw)/fps, score, s_e, s_h, s_v, s_f))
    return res

def voice_threshold(res):
    sc = np.array([x[2] for x in res])
    return float(np.clip(np.percentile(sc, 90), 0.35, 0.5))

def merge_ranges(res):
    thr = voice_threshold(res)
    out = []
    for a, b, sc, e, h, v, f in res:
        if sc >= thr:
            if out and abs(out[-1][1]-a) < 1e-6:
                out[-1][1] = b
                if sc > out[-1][2]: out[-1][2] = sc; out[-1][3] = (e, h, v, f)
            else: out.append([a, b, sc, (e, h, v, f)])
    return out
