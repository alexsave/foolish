# compare.py - measures how close a replica render is to the original window of a reference clip.
#
# What it does: aligns the replica to the original (one constant offset), then measures onsets, tempo and
#   micro-timing, harmony, pitch, drum pattern, spectrum and brightness, loudness, sidechain pump, and an
#   overall 0-100 score. Every metric is defined in README.md; none is lenient by construction (onset times are
#   never quantised, matching is one to one, chroma and CQT similarity are reported against a chance baseline).
# Inputs: audio files (anything ffmpeg decodes) or numpy arrays (compare()); optional stems drums/bass/other.
# Outputs: PREFIX.md (report), PREFIX.tsv (metric, value, unit, scope), PREFIX.png (plots). No JSON.
# How to run: python3 -I compare.py --orig A.mp3 --replica B.wav [--orig-start S --orig-end S ...]
#   [--orig-stems DIR --replica-stems DIR] [--no-align] [--out PREFIX]
# Known failure modes: the constant offset cannot follow a tempo mismatch (drift_ms shows it); onset detection on
#   dense pads is noisy; f0 on the "other" stem is polyphonic and untrustworthy; the greedy one-to-one matcher is
#   not a maximum matching (it takes the nearest pairs first).
# Randomness: none. Every step is deterministic.
import sys, os, argparse, subprocess, tempfile, math
import numpy as np
from scipy import signal, ndimage

SR = 22050            # analysis rate
HOP = 64              # onset-envelope hop at 22050 Hz = 2.9 ms
NFFT = 1024
TARGET_RMS = 0.1      # both sides are scaled to this overall RMS (same gain for a side's mix and stems)
SCOPES = ['mix', 'drums', 'bass', 'other', 'vocals']
NA = float('nan')


# ---------------------------------------------------------------- io
def to_mono(x):
    x = np.asarray(x, dtype=np.float64)
    if x.ndim == 2:
        x = x.mean(axis=0) if (x.shape[0] <= 8 and x.shape[1] > x.shape[0]) else x.mean(axis=1)
    return x


def decode(path, sr=44100):
    """Mono float64 at sr via ffmpeg (stereo is summed to mono by ffmpeg's downmix)."""
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-f', 'f32le', '-ac', '1', '-ar', str(sr), '-'],
                         capture_output=True).stdout
    if not raw:
        raise RuntimeError('ffmpeg could not decode ' + path)
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64), sr


def resample(x, sr, to=SR):
    if sr == to:
        return x
    g = math.gcd(int(sr), int(to))
    return signal.resample_poly(x, int(to) // g, int(sr) // g)


def rms(x):
    return float(np.sqrt(np.mean(np.square(x)))) if len(x) else 0.0


def db(x, floor=1e-12):
    return 10 * np.log10(np.maximum(x, floor))


# ---------------------------------------------------------------- spectrogram and onsets
def stft_mag(x, n, hop):
    """Amplitude STFT (Hann), frame i centred on sample i*hop; returns (frames, n//2+1) float32."""
    xp = np.pad(x, (n // 2, n // 2), mode='reflect' if len(x) > n // 2 else 'constant')   # reflect: no fake onset at the edges
    v = np.lib.stride_tricks.sliding_window_view(xp, n)[::hop]
    win = np.hanning(n)
    out = [np.abs(np.fft.rfft(v[i:i + 1024] * win, axis=1)).astype(np.float32) for i in range(0, len(v), 1024)]
    return np.concatenate(out) * np.float32(2.0 / win.sum())


def onset_env(x, n=NFFT, hop=HOP):
    """Spectral flux of log-compressed magnitudes against the frequency-max-filtered previous frame."""
    L = np.log1p(stft_mag(x, n, hop) * np.float32(1e4))
    prev = ndimage.maximum_filter1d(np.vstack([L[:1], L[:-1]]), 3, axis=1)
    d = np.maximum(L - prev, 0).sum(axis=1)
    d[0] = 0
    return d.astype(np.float64)


def pick_peaks(env, fs, k=1.0, win_s=1.0, min_dist_s=0.02, floor=0.0):
    """Adaptive-threshold peak picking: height > local mean + k*local std (1 s window), at least floor.
    Returns (times in s with parabolic sub-frame refinement, strengths). Never quantised to a grid."""
    if len(env) < 5:
        return np.zeros(0), np.zeros(0)
    w = max(3, int(win_s * fs))
    m = ndimage.uniform_filter1d(env, w, mode='nearest')
    s = np.sqrt(np.maximum(ndimage.uniform_filter1d(env ** 2, w, mode='nearest') - m ** 2, 0))
    thr = np.maximum(m + k * s, floor)
    pk, _ = signal.find_peaks(env, height=thr, distance=max(1, int(round(min_dist_s * fs))))
    pk = pk[(pk > 0) & (pk < len(env) - 1)]
    y0, y1, y2 = env[pk - 1], env[pk], env[pk + 1]
    den = y0 - 2 * y1 + y2
    delta = np.where(den < 0, 0.5 * (y0 - y2) / np.where(den < 0, den, -1.0), 0.0)
    delta = np.clip(delta, -0.5, 0.5)
    return (pk + delta) / fs, env[pk]


def detect_onsets(x22):
    env = onset_env(x22)
    fs = SR / HOP
    floor = 0.05 * np.percentile(env, 99) if len(env) else 0.0
    t, w = pick_peaks(env, fs, floor=floor)
    return t, w, env


def match(a, b, tol):
    """One-to-one matching of sorted time arrays: all pairs within tol, nearest first, each used at most once.
    Returns (idx_a, idx_b) arrays."""
    if len(a) == 0 or len(b) == 0:
        return np.zeros(0, int), np.zeros(0, int)
    pairs = []
    for i, t in enumerate(a):
        lo, hi = np.searchsorted(b, t - tol), np.searchsorted(b, t + tol, 'right')
        for j in range(lo, hi):
            pairs.append((abs(b[j] - t), i, j))
    pairs.sort()
    ua, ub, ia, ib = set(), set(), [], []
    for _, i, j in pairs:
        if i in ua or j in ub:
            continue
        ua.add(i); ub.add(j); ia.append(i); ib.append(j)
    return np.array(ia, int), np.array(ib, int)


def prf(n_o, n_r, tp):
    """precision, recall, F. Both sides empty -> nan. One side empty -> 0 (never lenient)."""
    if n_o == 0 and n_r == 0:
        return NA, NA, NA
    p = tp / n_r if n_r else 0.0
    r = tp / n_o if n_o else 0.0
    f = 2 * tp / (n_o + n_r)
    return p, r, f


# ---------------------------------------------------------------- result store
class Store:
    def __init__(self):
        self.d = {'_units': {}, '_extra': {}}

    def put(self, scope, name, val, unit=''):
        key = scope + '.' + name if scope else name
        self.d[key] = val
        self.d['_units'][key] = unit


def onset_metrics(S, scope, to, tr):
    ex = S.d['_extra']
    S.put(scope, 'onset_n_orig', len(to), 'count')
    S.put(scope, 'onset_n_replica', len(tr), 'count')
    for tol_ms in (15, 30, 50):
        ia, ib = match(to, tr, tol_ms / 1000)
        p, r, f = prf(len(to), len(tr), len(ia))
        S.put(scope, 'onset_f%d' % tol_ms, f, 'F')
        if tol_ms == 30:
            S.put(scope, 'onset_precision', p, 'ratio')
            S.put(scope, 'onset_recall', r, 'ratio')
            ex[scope + '.pairs30'] = (ia, ib)
        if tol_ms == 50:
            dev = (tr[ib] - to[ia]) * 1000 if len(ia) else np.zeros(0)
            S.put(scope, 'onset_dev_mean_ms', float(dev.mean()) if len(dev) else NA, 'ms')
            S.put(scope, 'onset_dev_std_ms', float(dev.std()) if len(dev) else NA, 'ms')
            S.put(scope, 'onset_dev_mad_ms', float(np.median(np.abs(dev - np.median(dev)))) if len(dev) else NA, 'ms')


# ---------------------------------------------------------------- alignment
def _hp_env(x22):
    e = onset_env(x22, NFFT, 32)
    e = ndimage.gaussian_filter1d(e, 1.5)
    return e - ndimage.uniform_filter1d(e, int(0.3 * SR / 32), mode='nearest')


def _corr(eo, er, fs, i0, i1, maxlag_s=0.5):
    """r[m] = sum_t eo[t] * er[t + lag], lag = (m - ml) / fs over t in [i0, i1); positive lag = replica later."""
    ml = int(maxlag_s * fs)
    a = eo[i0:i1]
    lo, hi = i0 - ml, i1 + ml
    b = np.zeros(hi - lo)
    s0, s1 = max(lo, 0), min(hi, len(er))
    if s1 > s0:
        b[s0 - lo:s1 - lo] = er[s0:s1]
    return signal.correlate(b, a, mode='valid', method='fft'), ml


def _peak(r, ml, fs, pred=None, sigma=0.07):
    """Lag in s of the correlation peak (parabolic), optionally preferring peaks near pred (s)."""
    w = np.ones(len(r))
    if pred is not None:
        w = np.exp(-0.5 * (((np.arange(len(r)) - ml) / fs - pred) / sigma) ** 2)
    m = int(np.argmax((r - r.min()) * w))
    d = 0.0
    if 0 < m < len(r) - 1:
        den = r[m - 1] - 2 * r[m] + r[m + 1]
        if den < 0:
            d = float(np.clip(0.5 * (r[m - 1] - r[m + 1]) / den, -0.5, 0.5))
    return (m - ml + d) / fs


def estimate_offset(o22, r22):
    """One constant offset (ms, positive = replica late, +-500 ms, about 1 ms resolution) and drift_ms.
    Periodic material makes a single correlation peak ambiguous by whole grid steps, so the offset is also tracked over
    consecutive 2 s windows (the first window prefers the smallest offset, each later window the peak nearest the previous one, extrapolating the trend); drift_ms is
    the mean tracked offset of the first third minus that of the last third; the constant offset is the global
    correlation peak nearest the median tracked offset."""
    fs = SR / 32
    eo, er = _hp_env(o22), _hp_env(r22)
    n = min(len(eo), len(er))
    W = int(2.0 * fs)
    nwin = n // W
    tracked = []
    for k in range(nwin):
        r, ml = _corr(eo, er, fs, k * W, (k + 1) * W)
        pred, sg = None, 0.07
        if k == 0:
            pred, sg = 0.0, 0.3          # the first window prefers the smallest offset among equally good peaks
        elif k == 1:
            pred = tracked[0]
        elif k >= 2:
            pred = 2 * tracked[-1] - tracked[-2]
        tracked.append(_peak(r, ml, fs, pred, sg))
    r, ml = _corr(eo, er, fs, 0, n)
    med = float(np.median(tracked)) if tracked else None
    off = _peak(r, ml, fs, med, sigma=0.05)
    drift = NA
    if nwin >= 3:
        third = max(1, nwin // 3)
        drift = float(np.mean(tracked[:third]) - np.mean(tracked[-third:])) * 1000.0
    return off * 1000.0, drift


def shift(x, n):
    """Replica shifted earlier by n samples (n > 0 drops the start, n < 0 pads zeros)."""
    if n > 0:
        return x[n:]
    if n < 0:
        return np.concatenate([np.zeros(-n), x])
    return x


# ---------------------------------------------------------------- tempo and micro-timing
def tempo_est(env, fs):
    e = env - ndimage.uniform_filter1d(env, max(3, int(0.5 * fs)), mode='nearest')
    n = len(e)
    if n < int(2 * fs) or np.allclose(e, 0):
        return dict(bpm=NA, half=NA, double=NA, acf_main=NA, acf_half=NA, acf_double=NA)
    f = np.fft.rfft(e, 2 * n)
    acf = np.fft.irfft(f * np.conj(f))[:n]
    acf = acf / acf[0]
    lags = np.arange(n) / fs
    ok = (lags >= 60 / 220.0) & (lags <= 60 / 50.0)
    bpm_ax = np.where(lags > 0, 60 / np.maximum(lags, 1e-9), 1)
    prior = np.exp(-0.5 * (np.log2(np.maximum(bpm_ax, 1e-3) / 110.0)) ** 2)
    score = np.where(ok, acf * prior, -1)
    L0 = lags[int(np.argmax(score))]
    ks, ls = [], []
    for k in range(1, 8):
        tgt = k * L0
        lo, hi = int(tgt * 0.97 * fs), int(tgt * 1.03 * fs) + 1
        if hi >= n - 1:
            break
        j = lo + int(np.argmax(acf[lo:hi]))
        den = acf[j - 1] - 2 * acf[j] + acf[j + 1]
        d = float(np.clip(0.5 * (acf[j - 1] - acf[j + 1]) / den, -0.5, 0.5)) if den < 0 else 0.0
        ks.append(k); ls.append((j + d) / fs)
    ks, ls = np.array(ks, float), np.array(ls)
    tau = float((ks * ls).sum() / (ks * ks).sum()) if len(ks) else L0

    def at(l):
        j = int(round(l * fs))
        return float(acf[j - 2:j + 3].max()) if 2 <= j < n - 3 else NA
    return dict(bpm=60 / tau, half=30 / tau, double=120 / tau, acf_main=at(tau), acf_half=at(2 * tau), acf_double=at(tau / 2))


def fold_onsets(t, phi, step, s0):
    u = (t - phi) / step
    k = np.round(u).astype(int)
    return ((u - k) * step * 1000.0), (k - s0) % 4


def micro_timing(to, wo, tr, bpm):
    if not (bpm == bpm) or len(to) < 8:
        return None
    step = 60.0 / bpm / 4
    cands = np.arange(0, step, 0.0005)
    d = ((to[None, :] - cands[:, None] + step / 2) % step) - step / 2
    score = (wo[None, :] * np.exp(-0.5 * (d / 0.006) ** 2)).sum(1)
    phi = float(cands[int(np.argmax(score))])
    k = np.round((to - phi) / step).astype(int) % 4
    wk = np.array([wo[k == s].sum() for s in range(4)])
    s0 = int(np.argmax(wk))
    out = {}
    for name, t in (('orig', to), ('replica', tr)):
        dev, slot = fold_onsets(t, phi, step, s0)
        prof = np.array([dev[slot == s].mean() if (slot == s).any() else NA for s in range(4)])
        share = np.array([(slot == s).mean() if len(slot) else NA for s in range(4)])
        odd, even = prof[[1, 3]], prof[[0, 2]]
        sw = float(np.mean(odd[~np.isnan(odd)]) - np.mean(even[~np.isnan(even)])) if (~np.isnan(odd)).any() and (~np.isnan(even)).any() else NA
        out[name] = (prof, share, sw)
    po, pr = out['orig'][0], out['replica'][0]
    both = ~np.isnan(po) & ~np.isnan(pr)
    out['diff'] = float(np.abs(po[both] - pr[both]).mean()) if both.any() else NA
    return out


# ---------------------------------------------------------------- harmony
_CQT_W = {}
def cqt_weights(n, sr):
    key = (n, sr)
    if key not in _CQT_W:
        nb = int(np.floor(36 * np.log2(7000 / 55.0))) + 1
        fk = 55.0 * 2 ** (np.arange(nb) / 36.0)
        fr = np.arange(n // 2 + 1) * sr / n
        W = np.zeros((n // 2 + 1, nb), np.float32)
        pos = np.clip(np.searchsorted(fr, fk) - 1, 0, len(fr) - 2)
        fr_f = (fk - fr[pos]) / (fr[1] - fr[0])
        W[pos, np.arange(nb)] = 1 - fr_f
        W[pos + 1, np.arange(nb)] = fr_f
        midi = 69 + 12 * np.log2(fk / 440.0)
        pc = np.round(midi).astype(int) % 12
        C = np.zeros((nb, 12), np.float32)
        C[np.arange(nb), pc] = 1
        _CQT_W[key] = (W, C)
    return _CQT_W[key]


CQT_HOP = 1024
def cqt_frames(x22):
    n = 8192
    S = stft_mag(x22, n, CQT_HOP)
    W, C = cqt_weights(n, SR)
    Q = S @ W
    return Q, Q @ C


def _cos_series(A, B, skip_db=-60.0):
    na, nb = np.linalg.norm(A, axis=1), np.linalg.norm(B, axis=1)
    dot = (A * B).sum(1)
    cos = np.where((na > 0) & (nb > 0), dot / np.maximum(na * nb, 1e-30), 0.0)
    ea = 20 * np.log10(np.maximum(na / max(na.max(), 1e-30), 1e-12))
    eb = 20 * np.log10(np.maximum(nb / max(nb.max(), 1e-30), 1e-12))
    skip = (ea < skip_db) & (eb < skip_db)
    cos = np.where(skip, np.nan, cos)
    return cos


def _chance_lags(nfr):
    dur = nfr * CQT_HOP / SR
    lags_s = [7.0, 13.0, 19.0] if dur >= 40 else [dur * 7 / 42, dur * 13 / 42, dur * 19 / 42]
    return [max(1, int(round(l * SR / CQT_HOP))) % nfr for l in lags_s]


def harmony_metrics(S, scope, xo, xr):
    Qo, Co = cqt_frames(xo)
    Qr, Cr = cqt_frames(xr)
    n = min(len(Qo), len(Qr))
    Qo, Co, Qr, Cr = Qo[:n], Co[:n], Qr[:n], Cr[:n]
    Lo, Lr = np.log1p(Qo * 1e3), np.log1p(Qr * 1e3)
    ser_c = _cos_series(Co, Cr)
    ser_q = _cos_series(Lo, Lr)
    S.d['_extra'][scope + '.chroma_series'] = ser_c
    for name, A, B, ser in (('chroma_cos', Co, Cr, ser_c), ('cqt_cos', Lo, Lr, ser_q)):
        v = float(np.nanmean(ser)) if not np.all(np.isnan(ser)) else NA
        ch = []
        for lag in _chance_lags(n):
            s = _cos_series(A, np.roll(B, lag, axis=0))
            if not np.all(np.isnan(s)):
                ch.append(float(np.nanmean(s)))
        c = float(np.mean(ch)) if ch else NA
        S.put(scope, name, v, 'cos')
        S.put(scope, name + '_chance', c, 'cos')
        S.put(scope, name + '_above_chance', (v - c) / (1 - c) if (v == v and c == c and c < 1) else NA, 'ratio')


# ---------------------------------------------------------------- pitch (YIN)
def yin(x, sr, fmin, fmax, W=1024, hop=512, thr=0.15):
    taumax = int(sr / fmin) + 1
    taumin = max(2, int(sr / fmax))
    flen = W + taumax + 2
    xp = np.pad(x, (0, flen))
    v = np.lib.stride_tricks.sliding_window_view(xp, flen)[:len(x):hop]
    N = 1 << int(np.ceil(np.log2(flen + W)))
    f0 = np.zeros(len(v)); per = np.zeros(len(v)); en = np.zeros(len(v))
    for i0 in range(0, len(v), 256):
        fr = v[i0:i0 + 256]
        A = fr[:, :W]
        r = np.fft.irfft(np.conj(np.fft.rfft(A, N, axis=1)) * np.fft.rfft(fr, N, axis=1), N, axis=1)[:, :taumax + 1]
        c = np.concatenate([np.zeros((len(fr), 1)), np.cumsum(fr ** 2, axis=1)], axis=1)
        e0 = c[:, W]
        et = c[:, W:W + taumax + 1] - c[:, :taumax + 1]
        d = e0[:, None] + et - 2 * r
        d[:, 0] = 0
        cm = np.ones_like(d)
        cs = np.cumsum(d[:, 1:], axis=1)
        cm[:, 1:] = d[:, 1:] * np.arange(1, taumax + 1)[None, :] / np.maximum(cs, 1e-30)
        for j in range(len(fr)):
            row = cm[j]
            en[i0 + j] = np.sqrt(e0[j] / W)
            below = np.where(row[taumin:taumax] < thr)[0]
            if len(below):
                t = taumin + below[0]
                while t + 1 < taumax and row[t + 1] < row[t]:
                    t += 1
            else:
                t = taumin + int(np.argmin(row[taumin:taumax]))
            per[i0 + j] = 1 - row[t]
            den = row[t - 1] - 2 * row[t] + row[t + 1]
            dd = float(np.clip(0.5 * (row[t - 1] - row[t + 1]) / den, -0.5, 0.5)) if den > 0 else 0.0
            f0[i0 + j] = sr / (t + dd)
    return f0, per, en


def pitch_metrics(S, scope, xo, xr, fmin, fmax, label=''):
    fo, po, eo = yin(xo, SR, fmin, fmax)
    fr, pr, er = yin(xr, SR, fmin, fmax)
    vo = (po > 0.75) & (eo > 10 ** (-45 / 20) * max(eo.max(), 1e-12))
    vr = (pr > 0.75) & (er > 10 ** (-45 / 20) * max(er.max(), 1e-12))
    S.put(scope, 'pitch_voiced_orig_frames', int(vo.sum()), 'count')
    if vo.sum() == 0:
        for nm in ('pitch_voiced_overlap', 'pitch_within50', 'pitch_within50_octave'):
            S.put(scope, nm, NA, 'ratio')
        return
    both = vo & vr
    S.put(scope, 'pitch_voiced_overlap', float(both.sum() / vo.sum()), 'ratio')
    if both.sum() == 0:
        S.put(scope, 'pitch_within50', 0.0, 'ratio'); S.put(scope, 'pitch_within50_octave', 0.0, 'ratio')
        return
    cents = 1200 * np.log2(fr[both] / fo[both])
    S.put(scope, 'pitch_within50', float((np.abs(cents) < 50).mean()), 'ratio')
    fold = (cents + 600) % 1200 - 600
    S.put(scope, 'pitch_within50_octave', float((np.abs(fold) < 50).mean()), 'ratio')


# ---------------------------------------------------------------- drum band streams
BANDS = {'kick': (30, 120, 0.025, 6.0), 'clap': (1500, 5000, 0.008, 6.0), 'hat': (7000, 11000, 0.006, 4.0)}


def band_db(x, sr, lo, hi, win_s):
    nyq = sr / 2
    if hi >= 0.98 * nyq:
        sos = signal.butter(4, lo / nyq, 'high', output='sos')
    else:
        sos = signal.butter(4, [lo / nyq, hi / nyq], 'band', output='sos')
    y = signal.sosfiltfilt(sos, x) if len(x) > 64 else x
    hop = int(round(sr * HOP / SR))
    w = max(hop, int(win_s * sr))
    c = np.concatenate([[0], np.cumsum(y ** 2)])
    ctr = np.arange(0, len(y), hop)
    a, b = np.clip(ctr - w // 2, 0, len(y)), np.clip(ctr + w // 2, 0, len(y))
    e = (c[b] - c[a]) / np.maximum(b - a, 1)
    return db(e), hop


def band_streams(S, scope, xo, xr, sr):
    """kick/clap/hat onset streams. The dB floor is relative to the louder side's peak (shared), so a replica that
    lacks a band does not get its noise floor amplified into onsets."""
    ex = S.d['_extra']
    for name, (lo, hi, win, rise) in BANDS.items():
        eo, hop = band_db(xo, sr, lo, hi, win)
        er, _ = band_db(xr, sr, lo, hi, win)
        ref = max(eo.max(), er.max())
        eo, er = np.maximum(eo, ref - 45), np.maximum(er, ref - 45)
        fs = sr / hop
        res = []
        for e in (eo, er):
            lag = 3
            d = np.zeros_like(e)
            d[lag:] = np.maximum(e[lag:] - e[:-lag], 0)
            t, w = pick_peaks(d, fs, min_dist_s=0.02, floor=rise)
            n50 = max(1, int(0.05 * fs))
            st = np.array([e[int(round(ti * fs)):int(round(ti * fs)) + n50 + 1].max() for ti in t]) if len(t) else np.zeros(0)
            res.append((t, st))
        (to, so), (tr, sr_) = res
        ex[scope + '.' + name + '_times'] = (to, tr)
        S.put(scope, name + '_n_orig', len(to), 'count')
        S.put(scope, name + '_n_replica', len(tr), 'count')
        ia, ib = match(to, tr, 0.03)
        p, r, f = prf(len(to), len(tr), len(ia))
        S.put(scope, name + '_precision', p, 'ratio')
        S.put(scope, name + '_recall', r, 'ratio')
        S.put(scope, name + '_f30', f, 'F')
        if len(ia) >= 3:
            a, b = so[ia], sr_[ib]
            corr = float(np.corrcoef(a, b)[0, 1]) if a.std() > 1e-9 and b.std() > 1e-9 else NA
            dd = b - a
            S.put(scope, name + '_strength_corr', corr, 'r')
            S.put(scope, name + '_strength_absdiff_db', float(np.abs(dd - np.median(dd)).mean()), 'dB')
        else:
            S.put(scope, name + '_strength_corr', NA, 'r')
            S.put(scope, name + '_strength_absdiff_db', NA, 'dB')


# ---------------------------------------------------------------- spectrum, brightness, loudness
THIRD = [31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500,
         3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000]


def lts(x, sr):
    nper = min(1 << 14, 1 << int(np.log2(max(len(x), 256))))
    f, P = signal.welch(x, fs=sr, nperseg=nper, window='hann')
    df = f[1] - f[0]
    out = np.full(len(THIRD), np.nan)
    for i, c in enumerate(THIRD):
        lo, hi = c / 2 ** (1 / 6), c * 2 ** (1 / 6)
        if hi > 0.98 * sr / 2:
            continue
        m = (f >= lo) & (f < hi)
        out[i] = P[m].sum() * df if m.any() else np.nan
    tot = np.nansum(out)
    return db(out / max(tot, 1e-30), 1e-30)


def pearson(a, b):
    if len(a) < 3 or a.std() < 1e-12 or b.std() < 1e-12:
        return NA
    return float(np.corrcoef(a, b)[0, 1])


def spectral_metrics(S, scope, xo22, xr22, xo, xr, sr):
    lo_, lr_ = lts(xo, sr), lts(xr, sr)
    diff = lr_ - lo_
    ok = ~np.isnan(lo_) & (lo_ >= np.nanmax(lo_) - 50)
    S.put(scope, 'lts_mad_db', float(np.abs(diff[ok]).mean()) if ok.any() else NA, 'dB')
    S.d['_extra'][scope + '.lts'] = (lo_, lr_)
    for i, c in enumerate(THIRD):
        S.put(scope, 'lts_diff_%gHz' % c, float(diff[i]) if diff[i] == diff[i] else NA, 'dB')
    # brightness: centroid per 100 ms frame
    hop = SR // 10
    fr = np.arange(1025) * SR / 2048
    cen, lev = [], []
    for x in (xo22, xr22):
        M = stft_mag(x, 2048, hop)
        cen.append((M * fr).sum(1) / np.maximum(M.sum(1), 1e-12))
        lev.append(db((M ** 2).sum(1)))
    n = min(len(cen[0]), len(cen[1]))
    co, cr, lo2, lr2 = cen[0][:n], cen[1][:n], lev[0][:n], lev[1][:n]
    valid = (lo2 > lo2.max() - 50) | (lr2 > lr2.max() - 50)
    S.put(scope, 'bright_corr', pearson(co[valid], cr[valid]), 'r')
    S.put(scope, 'bright_median_ratio', float(np.median(cr[valid] / np.maximum(co[valid], 1e-9))) if valid.any() else NA, 'ratio')
    # loudness
    def curve(x, win_s, hop_s):
        w, h = int(win_s * SR), int(hop_s * SR)
        c = np.concatenate([[0], np.cumsum(x ** 2)])
        a = np.arange(0, max(len(x) - w, 1), h)
        return db((c[a + w] - c[a]) / w)
    for tag, w, h in (('loud400', 0.4, 0.1), ('loud20', 0.02, 0.01)):
        co_, cr_ = curve(xo22, w, h), curve(xr22, w, h)
        n = min(len(co_), len(cr_))
        co_, cr_ = co_[:n], cr_[:n]
        fo_, fr_ = co_.max() - 60, cr_.max() - 60
        co_, cr_ = np.maximum(co_, fo_), np.maximum(cr_, fr_)
        valid = (co_ > co_.max() - 50) | (cr_ > cr_.max() - 50)
        if tag == 'loud400':
            S.put(scope, 'loud400_corr', pearson(co_[valid], cr_[valid]), 'r')
            S.put(scope, 'loud400_mad_db', float(np.abs(co_[valid] - cr_[valid]).mean()) if valid.any() else NA, 'dB')
            S.d['_extra'][scope + '.loud400'] = (co_, cr_)
        else:
            S.put(scope, 'loud20_corr', pearson(co_[valid], cr_[valid]), 'r')


# ---------------------------------------------------------------- sidechain
def level_curve(x, sr, a0, a1, step):
    c = np.concatenate([[0], np.cumsum(x ** 2)])
    def lv(t0, t1):
        i0, i1 = int(t0 * sr), int(t1 * sr)
        return db((c[i1] - c[i0]) / max(i1 - i0, 1))
    return lv


def pump_side(x, sr, kicks):
    lv = level_curve(x, sr, 0, 0, 0)
    pumps, recs = [], []
    for t in kicks:
        if t + 0.45 > len(x) / sr:
            continue
        L1, L2 = lv(t + 0.02, t + 0.08), lv(t + 0.15, t + 0.25)
        pumps.append(L1 - L2)
        ts = np.arange(0, 0.40, 0.005)
        cur = np.array([lv(t + s, t + s + 0.01) for s in ts])
        ok = cur >= L2 - 1.0
        first = [i for i in range(len(ok) - 10) if ok[i:i + 10].all()]   # within 1 dB and staying for 50 ms
        recs.append(first[0] * 5.0 if first else NA)
    recs = [r for r in recs if r == r]
    return (float(np.median(pumps)) if pumps else NA), (float(np.median(recs)) if recs else NA)


def sidechain_metrics(S, scope, nat_o, nat_r, sr, drum_scope):
    ex = S.d['_extra']
    kt = ex.get(drum_scope + '.kick_times')
    ok = all(k in nat_o and k in nat_r for k in ('other', 'bass')) and kt is not None
    if not ok:
        for nm in ('pump_db_orig', 'pump_db_replica', 'pump_recovery_ms_orig', 'pump_recovery_ms_replica'):
            S.put('mix', nm, NA, 'dB' if 'db' in nm else 'ms')
        return
    po, ro = pump_side(nat_o['other'] + nat_o['bass'], sr, kt[0])
    pr, rr = pump_side(nat_r['other'] + nat_r['bass'], sr, kt[1])
    S.put('mix', 'pump_db_orig', po, 'dB'); S.put('mix', 'pump_db_replica', pr, 'dB')
    S.put('mix', 'pump_recovery_ms_orig', ro, 'ms'); S.put('mix', 'pump_recovery_ms_replica', rr, 'ms')


# ---------------------------------------------------------------- compare
def _mean(vals):
    v = [x for x in vals if x == x and x is not None]
    return float(np.mean(v)) if v else NA


def compare(orig, replica, sr, stems_orig=None, stems_replica=None, align=True, joins=None):
    S = Store()
    ex = S.d['_extra']
    mo, mr = to_mono(orig), to_mono(replica)
    stems_o = {k: to_mono(v) for k, v in (stems_orig or {}).items()}
    stems_r = {k: to_mono(v) for k, v in (stems_replica or {}).items()}
    have = [k for k in ('drums', 'bass', 'other', 'vocals') if k in stems_o and k in stems_r]

    # 0. alignment (on equal-RMS signals)
    gain_o = TARGET_RMS / max(rms(mo), 1e-12)
    gain_r = TARGET_RMS / max(rms(mr), 1e-12)
    off_ms, drift_ms = estimate_offset(resample(mo * gain_o, sr), resample(mr * gain_r, sr))
    S.put('', 'offset_ms', off_ms, 'ms')
    S.put('', 'drift_ms', drift_ms, 'ms')
    ns = int(round(off_ms / 1000 * sr)) if align else 0
    S.put('', 'aligned', bool(align), 'flag')
    mr = shift(mr, ns)
    stems_r = {k: shift(v, ns) for k, v in stems_r.items()}
    n = min(len(mo), len(mr), *[len(stems_o[k]) for k in have], *[len(stems_r[k]) for k in have])
    nat_o = {'mix': mo[:n] * gain_o}
    nat_r = {'mix': mr[:n] * gain_r}
    for k in have:
        nat_o[k] = stems_o[k][:n] * gain_o
        nat_r[k] = stems_r[k][:n] * gain_r
    S.put('', 'duration_s', n / sr, 's')
    # silent stems (orig side silent) are n/a
    silent = {k: rms(nat_o[k]) < 1e-4 * rms(nat_o['mix']) for k in have}
    scopes = ['mix'] + [k for k in have if not silent[k]]
    ex['scopes'] = scopes
    ex['silent'] = [k for k in have if silent[k]]
    x22 = {'o': {k: resample(v, sr) for k, v in nat_o.items()}, 'r': {k: resample(v, sr) for k, v in nat_r.items()}}

    # 1. onsets
    env_mix = {}
    ons = {}
    for sc in scopes:
        if sc == 'vocals':
            continue
        to, wo, eo = detect_onsets(x22['o'][sc])
        tr, wr, er = detect_onsets(x22['r'][sc])
        ons[sc] = (to, wo, tr, wr)
        onset_metrics(S, sc, to, tr)
        if sc == 'mix':
            ex['env_mix'] = (eo, er)
            ex['onsets_mix'] = (to, tr)

    # 2. tempo and micro-timing
    fs_env = SR / HOP
    for sc in [s for s in ('mix', 'drums') if s in scopes]:
        eo = onset_env(x22['o'][sc]); er = onset_env(x22['r'][sc])
        to_ = tempo_est(eo, fs_env); tr_ = tempo_est(er, fs_env)
        S.put(sc, 'tempo_bpm_orig', to_['bpm'], 'BPM')
        S.put(sc, 'tempo_bpm_replica', tr_['bpm'], 'BPM')
        S.put(sc, 'tempo_pct_diff', (tr_['bpm'] / to_['bpm'] - 1) * 100 if to_['bpm'] == to_['bpm'] and tr_['bpm'] == tr_['bpm'] else NA, '%')
        for nm, t_ in (('orig', to_), ('replica', tr_)):
            S.put(sc, 'tempo_half_candidate_%s' % nm, t_['half'], 'BPM')
            S.put(sc, 'tempo_double_candidate_%s' % nm, t_['double'], 'BPM')
            S.put(sc, 'tempo_acf_main_%s' % nm, t_['acf_main'], 'ratio')
            S.put(sc, 'tempo_acf_half_bpm_%s' % nm, t_['acf_half'], 'ratio')
            S.put(sc, 'tempo_acf_double_bpm_%s' % nm, t_['acf_double'], 'ratio')
        to, wo, tr, wr = ons[sc]
        mt = micro_timing(to, wo, tr, to_['bpm'])
        if mt is not None:
            S.put(sc, 'swing_ms_orig', mt['orig'][2], 'ms')
            S.put(sc, 'swing_ms_replica', mt['replica'][2], 'ms')
            S.put(sc, 'microtiming_profile_mad_ms', mt['diff'], 'ms')
            for s in range(4):
                S.put(sc, 'micro_dev_orig_slot%d' % s, float(mt['orig'][0][s]), 'ms')
                S.put(sc, 'micro_dev_replica_slot%d' % s, float(mt['replica'][0][s]), 'ms')
                S.put(sc, 'micro_share_orig_slot%d' % s, float(mt['orig'][1][s]), 'ratio')
                S.put(sc, 'micro_share_replica_slot%d' % s, float(mt['replica'][1][s]), 'ratio')
        else:
            S.put(sc, 'microtiming_profile_mad_ms', NA, 'ms')

    # 3. harmony
    for sc in [s for s in ('mix', 'other', 'bass') if s in scopes]:
        harmony_metrics(S, sc, x22['o'][sc], x22['r'][sc])

    # 4. pitch
    if 'bass' in scopes:
        pitch_metrics(S, 'bass', x22['o']['bass'], x22['r']['bass'], 30, 400)
    if 'other' in scopes:
        pitch_metrics(S, 'other', x22['o']['other'], x22['r']['other'], 80, 800)

    # 5. drum pattern
    dsc = 'drums' if 'drums' in scopes else 'mix'
    ex['drum_scope'] = dsc
    band_streams(S, dsc, nat_o[dsc], nat_r[dsc], sr)

    # 6, 7. spectrum, brightness, loudness
    for sc in scopes:
        spectral_metrics(S, sc, x22['o'][sc], x22['r'][sc], nat_o[sc], nat_r[sc], sr)

    # 8. sidechain
    if 'drums' in scopes:
        sidechain_metrics(S, 'mix', nat_o, nat_r, sr, 'drums')
    else:
        sidechain_metrics(S, 'mix', nat_o, nat_r, sr, 'none')

    # glitch measures (additive; stems only)
    for sc in [k for k in scopes if k in ('drums', 'bass', 'other')]:
        for nm, (v, u) in glitch(nat_o[sc], nat_r[sc], sr, (joins or {}).get(sc)).items():
            S.put(sc, nm, v, u)

    # 9. summary
    d = S.d
    def g(k):
        v = d.get(k, NA)
        return NA if v is None else v
    terms = [('onset_f30_mix', g('mix.onset_f30')),
             ('onset_f30_drums_stem' if 'drums' in scopes else 'onset_f30_drums_stem(mix)', g('drums.onset_f30') if 'drums' in scopes else g('mix.onset_f30')),
             ('kick_f30', g(dsc + '.kick_f30')), ('clap_f30', g(dsc + '.clap_f30')), ('hat_f30', g(dsc + '.hat_f30'))]
    def clip01(v):
        return NA if v != v else float(np.clip(v, 0, 1))
    terms += [('chroma_above_chance', clip01(g('mix.chroma_cos_above_chance'))),
              ('cqt_above_chance', clip01(g('mix.cqt_cos_above_chance')))]
    if 'bass' in scopes and g('bass.pitch_within50') == g('bass.pitch_within50'):
        terms.append(('bass_pitch_within50_x_voiced', g('bass.pitch_within50') * g('bass.pitch_voiced_overlap')))
    else:
        terms.append(('bass_pitch_within50_x_voiced', NA))
    lm = g('mix.lts_mad_db')
    terms.append(('lts', NA if lm != lm else max(0.0, 1 - lm / 12.0)))
    lc = g('mix.loud400_corr')
    terms.append(('loudness_corr', NA if lc != lc else max(0.0, lc)))
    bc = g('mix.bright_corr')
    terms.append(('brightness_corr', NA if bc != bc else max(0.0, bc)))
    ex['terms'] = terms
    S.put('', 'overall', 100 * _mean([v for _, v in terms]), 'score')
    return S.d


# ---------------------------------------------------------------- glitch measures (iteration 3, additive: nothing above reads these)
GLITCH_ENV_MS = 8.0            # analytic-signal envelope smoothing
GLITCH_LAG_MS = 5.0            # best integer lag searched, +-
GLITCH_DIP_DB = 3.0            # a dip or bump is a replica envelope this far below or above the original's ...
GLITCH_RANGE_DB = 30.0         # ... while the original is within this far of its stem peak
GLITCH_JOIN_MS = 20.0          # a join's window, +-
GLITCH_STEP_DB, GLITCH_STEP_MS, GLITCH_STEP_ORIG_DB, GLITCH_STEP_ORIG_MS = 6.0, 10.0, 3.0, 20.0
GLITCH_CLICK_X, GLITCH_CLICK_RMS_MS, GLITCH_CLICK_NEAR_MS = 4.0, 5.0, 2.0
GLITCH_CLICK_FLOOR = 0.01     # a jump must also exceed this share of the original stem's peak (-40 dB), or silence with a trace of noise is full of clicks
GLITCH_FLOOR_DB = 60.0         # envelopes are floored this far below the original's peak
GLITCH_PER_LAGS_S = (0.05, 2.0)


def _env_db(x, sr, floor_db):
    e = np.abs(signal.hilbert(x))
    w = max(1, int(round(GLITCH_ENV_MS / 1000 * sr)))
    c = np.concatenate([[0.0], np.cumsum(e)])
    i = np.arange(len(e))
    a, b = np.clip(i - w // 2, 0, len(e)), np.clip(i + w - w // 2, 0, len(e))
    sm = (c[b] - c[a]) / np.maximum(b - a, 1)
    return 20 * np.log10(np.maximum(sm, 10 ** (floor_db / 20))), sm


def _runs(mask):
    """Number of contiguous True runs."""
    m = np.concatenate([[0], mask.astype(np.int8), [0]])
    return int((np.diff(m) == 1).sum())


def _align_gain(xo, xr, sr, do_align=True, do_gain=True):
    n = min(len(xo), len(xr))
    xo, xr = xo[:n], xr[:n]
    lag = 0
    if do_align and n > 16:
        ml = int(round(GLITCH_LAG_MS / 1000 * sr))
        N = 1 << int(np.ceil(np.log2(2 * n)))
        cc = np.fft.irfft(np.fft.rfft(xo, N) * np.conj(np.fft.rfft(xr, N)), N)
        cand = np.concatenate([cc[:ml + 1], cc[N - ml:]])
        lags = np.concatenate([np.arange(0, ml + 1), np.arange(-ml, 0)])
        lag = int(lags[int(np.argmax(cand))])     # replica sample i matches original sample i + lag
    r = np.zeros(n)
    if lag >= 0:
        r[lag:] = xr[:n - lag]
    else:
        r[:lag] = xr[-lag:]
    g = float(xo @ r) / max(float(r @ r), 1e-30) if do_gain else 1.0
    return xo, r * g, lag, g


def glitch(xo, xr, sr, joins=None):
    """Glitch measures of one stem (mono, same rate on both sides). Returns an ordered dict name -> (value, unit)."""
    out = {}
    xo, r, lag, g = _align_gain(np.asarray(xo, float), np.asarray(xr, float), sr)
    n = len(xo)
    out['glitch_lag_ms'] = (lag / sr * 1000, 'ms')
    out['glitch_gain_db'] = (20 * np.log10(max(g, 1e-12)), 'dB')
    eo_pow = float(np.sum(xo ** 2))
    out['glitch_null_residual_db'] = (10 * np.log10(max(float(np.sum((xo - r) ** 2)), 1e-30) / max(eo_pow, 1e-30)), 'dB')
    peak_db = 20 * np.log10(max(float(np.max(np.abs(signal.hilbert(xo)))), 1e-12))
    eo, _ = _env_db(xo, sr, peak_db - GLITCH_FLOOR_DB)
    er, _ = _env_db(r, sr, peak_db - GLITCH_FLOOR_DB)
    eo_peak = float(eo.max())
    diff = er - eo
    act = eo > eo_peak - GLITCH_RANGE_DB
    dip = act & (diff < -GLITCH_DIP_DB)
    bump = act & (diff > GLITCH_DIP_DB)
    out['glitch_dip_frames'] = (_runs(dip), 'count')
    out['glitch_dip_s'] = (float(dip.sum()) / sr, 's')
    out['glitch_bump_frames'] = (_runs(bump), 'count')
    out['glitch_bump_s'] = (float(bump.sum()) / sr, 's')
    out['glitch_deepest_dip_db'] = (float(diff[dip].min()) if dip.any() else 0.0, 'dB')
    if joins is not None:
        jm = []
        w = int(round(GLITCH_JOIN_MS / 1000 * sr))
        for t in joins:
            i = int(round(t * sr))
            if i < 0 or i >= n:
                continue
            jm.append(float(diff[max(0, i - w):i + w + 1].min()))
        jm = np.array(jm)
        out['glitch_joins'] = (len(jm), 'count')
        out['glitch_joins_over1db'] = (int((jm < -1).sum()), 'count')
        out['glitch_joins_over3db'] = (int((jm < -3).sum()), 'count')
        out['glitch_joins_deepest_db'] = (float(jm.min()) if len(jm) else 0.0, 'dB')
        out['glitch_joins_median_db'] = (float(np.median(jm)) if len(jm) else 0.0, 'dB')
    # level steps: on a 1 ms grid
    h = max(1, int(round(sr / 1000)))
    e1, r1 = eo[::h], er[::h]
    k10 = int(round(GLITCH_STEP_MS)); k20 = int(round(GLITCH_STEP_ORIG_MS))
    m = len(e1)
    step = np.zeros(m, bool)
    if m > k10 + 1:
        big = np.abs(r1[k10:] - r1[:-k10]) > GLITCH_STEP_DB
        from scipy.ndimage import maximum_filter1d, minimum_filter1d
        rng_o = maximum_filter1d(e1, k20, mode='nearest') - minimum_filter1d(e1, k20, mode='nearest')
        calm = rng_o[:m - k10] < GLITCH_STEP_ORIG_DB
        step[:m - k10] = big & calm
    out['glitch_level_steps'] = (_runs(step), 'count')
    # clicks
    wc = max(1, int(round(GLITCH_CLICK_RMS_MS / 1000 * sr)))
    def jumps(x):
        d = np.abs(np.diff(x, prepend=x[0]))
        c = np.concatenate([[0.0], np.cumsum(x ** 2)])
        i = np.arange(len(x))
        a, b = np.clip(i - wc // 2, 0, len(x)), np.clip(i + wc - wc // 2, 0, len(x))
        rms_ = np.sqrt((c[b] - c[a]) / np.maximum(b - a, 1))
        return (d > GLITCH_CLICK_X * rms_) & (d > GLITCH_CLICK_FLOOR * max(float(np.abs(xo).max()), 1e-12))
    jr, jo = jumps(r), jumps(xo)
    near = max(1, int(round(GLITCH_CLICK_NEAR_MS / 1000 * sr)))
    jo_near = np.convolve(jo.astype(np.int32), np.ones(2 * near + 1, np.int32), mode='same') > 0
    cl = jr & ~jo_near
    # clicks within 2 ms of each other are one click
    idx = np.nonzero(cl)[0]
    out['glitch_clicks'] = (int(1 + (np.diff(idx) > near).sum()) if len(idx) else 0, 'count')
    # regularly recurring faults: the residual's 10 ms envelope autocorrelation
    f10 = max(1, int(round(0.01 * sr)))
    nf = n // f10
    res = (xo - r)[:nf * f10].reshape(nf, f10)
    ev = np.sqrt(np.mean(res ** 2, axis=1)); ev = ev - ev.mean()
    lo, hi = int(round(GLITCH_PER_LAGS_S[0] / 0.01)), min(int(round(GLITCH_PER_LAGS_S[1] / 0.01)), nf - 2)
    pk, pl = 0.0, 0.0
    if hi > lo and float(ev @ ev) > 0:
        ac = np.array([float(ev[:nf - L] @ ev[L:]) / float(ev @ ev) for L in range(lo - 1, hi + 2)])
        for j in range(1, len(ac) - 1):
            if ac[j] >= ac[j - 1] and ac[j] > ac[j + 1] and ac[j] > pk:
                pk, pl = float(ac[j]), (lo - 1 + j) * 0.01
    out['glitch_residual_periodicity'] = (pk, 'r')
    out['glitch_residual_periodicity_lag_s'] = (pl, 's')
    return out


# ---------------------------------------------------------------- report
def worst_windows(R, win_s=2.0):
    ex = R['_extra']
    ser = ex.get('mix.chroma_series')
    fr_s = CQT_HOP / SR
    rows_c, rows_f = [], []
    dur = R['duration_s']
    nw = int(np.ceil(dur / win_s))
    to, tr = ex['onsets_mix']
    ia, ib = ex['mix.pairs30']
    for w in range(nw):
        a, b = w * win_s, (w + 1) * win_s
        if ser is not None:
            seg = ser[int(a / fr_s):int(b / fr_s)]
            if len(seg) and not np.all(np.isnan(seg)):
                rows_c.append((float(np.nanmean(seg)), a, b))
        no = int(((to >= a) & (to < b)).sum()); nr = int(((tr >= a) & (tr < b)).sum())
        tp = int(((to[ia] >= a) & (to[ia] < b)).sum())
        if no + nr:
            rows_f.append((2 * tp / (no + nr), a, b, no, nr, tp))
    return sorted(rows_c)[:10], sorted(rows_f)[:10]


def fmt(v):
    if isinstance(v, (bool, np.bool_)):
        return str(bool(v))
    if isinstance(v, (int, np.integer)):
        return str(int(v))
    if v is None or v != v:
        return 'n/a'
    return '%.4g' % v


def write_report(R, prefix, title_info=''):
    units = R['_units']
    ex = R['_extra']
    keys = [k for k in R if not k.startswith('_')]
    # tsv
    with open(prefix + '.tsv', 'w') as f:
        f.write('metric\tvalue\tunit\tscope\n')
        for k in keys:
            sc, _, nm = k.partition('.') if k.split('.')[0] in SCOPES else ('global', '', k)
            f.write('%s\t%s\t%s\t%s\n' % (nm, fmt(R[k]), units.get(k, ''), sc))
    L = []
    L.append('# Replica comparison')
    L.append('')
    if title_info:
        L.append(title_info)
        L.append('')
    L.append('Overall score: **%s / 100**' % fmt(R['overall']))
    L.append('')
    L.append('The overall score is the mean (x100) of these terms; a term that is n/a is left out of the mean.')
    L.append('')
    L.append('| term | value |')
    L.append('|---|---|')
    for nm, v in ex['terms']:
        L.append('| %s | %s |' % (nm, fmt(v)))
    L.append('')
    L.append('Formula: overall = 100 * mean(onset F@30 mix, drum-stem onset F@30 (mix if no stems), kick F@30, clap F@30, '
             'hat F@30, clip01(chroma_cos_above_chance), clip01(cqt_cos_above_chance), bass pitch_within50 * pitch_voiced_overlap '
             '(omitted without a bass stem), max(0, 1 - lts_mad_db/12), max(0, loudness 400 ms correlation), max(0, brightness correlation)).')
    L.append('')
    L.append('Alignment: offset_ms = %s (positive: replica late; applied: %s), drift_ms = %s (offset on the first third minus the last third), duration compared %s s.'
             % (fmt(R['offset_ms']), R['aligned'], fmt(R['drift_ms']), fmt(R['duration_s'])))
    if ex['silent']:
        L.append('')
        L.append('Silent stems in the original (all their metrics are n/a): %s.' % ', '.join(ex['silent']))
    for sc in SCOPES:
        ks = [k for k in keys if k.startswith(sc + '.')]
        if not ks:
            continue
        L.append('')
        L.append('## Scope: %s' % sc)
        if sc == 'other':
            L.append('')
            L.append('The pitch rows of this scope are untrustworthy: polyphonic material.')
        L.append('')
        L.append('| metric | value | unit | scope |')
        L.append('|---|---|---|---|')
        for k in [k for k in ks if not k[len(sc) + 1:].startswith('glitch_')]:
            L.append('| %s | %s | %s | %s |' % (k[len(sc) + 1:], fmt(R[k]), units.get(k, ''), sc))
    gk = [k for k in keys if '.glitch_' in k]
    if gk:
        L.append('')
        L.append('## Glitch measures')
        L.append('')
        L.append('| metric | value | unit | scope |')
        L.append('|---|---|---|---|')
        for k in gk:
            L.append('| %s | %s | %s | %s |' % (k.split('.', 1)[1], fmt(R[k]), units.get(k, ''), k.split('.')[0]))
    wc, wf = worst_windows(R)
    L.append('')
    L.append('## Worst 2 s windows by chroma similarity (mix)')
    L.append('')
    L.append('| start s | end s | mean chroma cos |')
    L.append('|---|---|---|')
    for v, a, b in wc:
        L.append('| %.1f | %.1f | %.3f |' % (a, b, v))
    L.append('')
    L.append('## Worst 2 s windows by onset F@30 (mix)')
    L.append('')
    L.append('| start s | end s | F@30 | orig onsets | replica onsets | matched |')
    L.append('|---|---|---|---|---|---|')
    for v, a, b, no, nr, tp in wf:
        L.append('| %.1f | %.1f | %.3f | %d | %d | %d |' % (a, b, v, no, nr, tp))
    L.append('')
    with open(prefix + '.md', 'w') as f:
        f.write('\n'.join(L) + '\n')


def write_plot(R, prefix):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    ex = R['_extra']
    eo, er = ex['env_mix']
    to, tr = ex['onsets_mix']
    ia, ib = ex['mix.pairs30']
    fs = SR / HOP
    t = np.arange(len(eo)) / fs
    fig, ax = plt.subplots(4, 1, figsize=(14, 13))
    a = ax[0]
    a.plot(t, eo, color='tab:blue', lw=0.6, label='original')
    a.plot(np.arange(len(er)) / fs, -er, color='tab:orange', lw=0.6, label='replica (mirrored)')
    mo = np.zeros(len(to), bool); mo[ia] = True
    mr = np.zeros(len(tr), bool); mr[ib] = True
    top = max(eo.max(), er.max())
    a.plot(to[mo], np.full(mo.sum(), top * 1.05), 'g.', ms=4, label='matched')
    a.plot(to[~mo], np.full((~mo).sum(), top * 1.05), 'rx', ms=4, label='missed (orig only)')
    a.plot(tr[mr], np.full(mr.sum(), -top * 1.05), 'g.', ms=4)
    a.plot(tr[~mr], np.full((~mr).sum(), -top * 1.05), 'm+', ms=5, label='extra (replica only)')
    a.set_title('Onset envelopes (mix), F@30 = %s' % fmt(R['mix.onset_f30'])); a.legend(loc='upper right', ncol=5, fontsize=7)
    a = ax[1]
    ser = ex.get('mix.chroma_series')
    if ser is not None:
        a.plot(np.arange(len(ser)) * CQT_HOP / SR, ser, color='k', lw=0.8)
        a.axhline(R['mix.chroma_cos'], color='b', ls='--', lw=0.6, label='mean')
        a.axhline(R['mix.chroma_cos_chance'], color='r', ls=':', lw=0.8, label='chance')
        a.legend(fontsize=7)
    a.set_ylim(0, 1.02); a.set_title('Chroma cosine per frame (mix)')
    a = ax[2]
    co, cr = ex['mix.loud400']
    a.plot(np.arange(len(co)) * 0.1, co, label='original'); a.plot(np.arange(len(cr)) * 0.1, cr, label='replica')
    a.set_title('Loudness, 400 ms RMS in dB (corr %s)' % fmt(R['mix.loud400_corr'])); a.legend(fontsize=7)
    a = ax[3]
    lo_, lr_ = ex['mix.lts']
    a.semilogx(THIRD, lo_, 'o-', label='original'); a.semilogx(THIRD, lr_, 's-', label='replica')
    a.set_title('Third-octave long-term spectrum, equal total level (mad %s dB)' % fmt(R['mix.lts_mad_db'])); a.legend(fontsize=7)
    fig.tight_layout()
    fig.savefig(prefix + '.png', dpi=90)
    plt.close(fig)


# ---------------------------------------------------------------- cli
def _load_window(path, start, end):
    x, sr = decode(path)
    a = int(start * sr) if start is not None else 0
    b = int(end * sr) if end is not None else len(x)
    return x[a:b], sr


def _load_stems(d, start, end, sr):
    out = {}
    for k in ('drums', 'bass', 'other', 'vocals'):
        p = os.path.join(d, k + '.wav')
        if os.path.exists(p):
            x, s = _load_window(p, start, end)
            out[k] = x
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description='Measure how close a replica render is to an original window.')
    ap.add_argument('--orig', required=True)
    ap.add_argument('--replica', required=True)
    ap.add_argument('--orig-start', type=float); ap.add_argument('--orig-end', type=float)
    ap.add_argument('--replica-start', type=float); ap.add_argument('--replica-end', type=float)
    ap.add_argument('--orig-stems'); ap.add_argument('--replica-stems')
    ap.add_argument('--no-align', action='store_true')
    for _k in ('drums', 'bass', 'other'):
        ap.add_argument('--joins-' + _k, help='file with one join time (s, relative to the window) per line: adds the glitch join metrics of that stem')
    ap.add_argument('--out', default='compare_out')
    a = ap.parse_args(argv)
    xo, sr = _load_window(a.orig, a.orig_start, a.orig_end)
    xr, sr2 = _load_window(a.replica, a.replica_start, a.replica_end)
    so = _load_stems(a.orig_stems, a.orig_start, a.orig_end, sr) if a.orig_stems else None
    sr_ = _load_stems(a.replica_stems, a.replica_start, a.replica_end, sr) if a.replica_stems else None
    if (so is None) != (sr_ is None):
        so = sr_ = None
        print('stems given for one side only: ignored', file=sys.stderr)
    joins = {k: np.array([float(l) for l in open(getattr(a, 'joins_' + k)) if l.strip()]) for k in ('drums', 'bass', 'other') if getattr(a, 'joins_' + k)}
    R = compare(xo, xr, sr, so, sr_, align=not a.no_align, joins=joins)
    info = 'Original: %s [%s-%s s]. Replica: %s [%s-%s s].' % (a.orig, a.orig_start, a.orig_end, a.replica, a.replica_start, a.replica_end)
    write_report(R, a.out, info)
    write_plot(R, a.out)
    print('overall %.2f' % R['overall'])
    for nm, v in R['_extra']['terms']:
        print('  %-32s %s' % (nm, fmt(v)))
    return R


if __name__ == '__main__':
    main()
