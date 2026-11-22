#!/usr/bin/env python3
"""citymix.py: the measuring and drawing half of the city mixes (splice.c renders).

  clip  id wav beats.txt stems_dir|- out.txt out.png
        one recording: a table with a row per bar (attack time of the downbeat, tempo, level, band levels, vocal
        stem level, chroma key, tuning, chord) and a picture with the numbered bar lines.
  look  wav t0 t1 out.png [title]
        a spectrogram and waveform of any stretch.
  sheet out.png in.png ...
        pictures stacked into one.
  seams wav seams.tsv timeline.tsv outdir name
        one picture per seam of a splice render (outdir/<name>_sNN.png), contact sheets of four
        (outdir/<name>_seamsheet_K.png) and a table of measures with a FLAGS column (outdir/<name>_seams.tsv, also
        on stdout): click, hole, step (overall and by band), beat grid offset, key distance.
  verify wav timeline.tsv
        proves the timeline: every row is found again in its source at the time the timeline says. Prints the
        rows that do not pass and one summary line; exit status 1 on any FAIL.
  song  wav seams.tsv out.png title
        the whole song in one picture, rows of 180 s, with the numbered seams.

Tables and pictures only; nothing here is a listening verdict."""
import subprocess
import sys
import numpy as np
import soundfile as sf
from scipy import signal
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

SR = 48000
NOTE = ['C', 'C#', 'D', 'Eb', 'E', 'F', 'F#', 'G', 'G#', 'A', 'Bb', 'B']
MAJ = np.array([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88])
MIN = np.array([6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17])


def db(p):
    return 10 * np.log10(np.maximum(p, 1e-12))


def load(path, t0=None, t1=None):
    i = sf.info(path)
    a = 0 if t0 is None else max(0, int(t0 * i.samplerate))
    b = i.frames if t1 is None else min(i.frames, int(t1 * i.samplerate))
    x, sr = sf.read(path, start=a, stop=b, dtype='float32', always_2d=True)
    return x, sr


def kweight(x, sr=SR):
    y = signal.lfilter([1.53512485958697, -2.69169618940638, 1.19839281085285],
                       [1, -1.69065929318241, 0.73248077421585], x, axis=0)
    return signal.lfilter([1, -2, 1], [1, -1.99004745483398, 0.99007225036621], y, axis=0)


def attack(m, sr, t, w=0.07):
    """The start of the strongest rise within +-w of t, on a 3 ms envelope (low band counted again)."""
    a, b = int((t - w - 0.03) * sr), int((t + w + 0.03) * sr)
    a, b = max(0, a), min(len(m), b)
    s = m[a:b]
    if len(s) < int(0.06 * sr):
        return t
    low = signal.lfilter(*signal.butter(2, 150, fs=sr), s)
    n = int(0.003 * sr)
    hop = max(1, n // 4)
    c = np.cumsum(np.concatenate(([0.0], (s * s + 3 * low * low).astype('float64'))))
    e = db((c[n::hop] - c[:-n:hop][:len(c[n::hop])]) / n + 1e-7)
    te = (a + n + np.arange(len(e)) * hop) / sr
    lag = max(1, int(0.015 * sr / hop))
    rise = e[lag:] - e[:-lag]
    ok = np.abs(te[lag:] - t) <= w
    if not ok.any() or rise[ok].max() < 1.5:
        return t
    i = np.where(ok)[0][np.argmax(rise[ok])]
    seg = e[i:i + lag + 1]
    j = np.argmax(seg >= seg[0] + 0.25 * (seg[-1] - seg[0]))
    return te[i + j] - 0.002


def logspec(m, sr, hop=512, nfft=4096, rows=220):
    f, t, z = signal.stft(m, sr, nperseg=nfft, noverlap=nfft - hop)
    p = np.abs(z) ** 2
    edges = np.geomspace(30, 16000, rows + 1)
    out = np.empty((rows, p.shape[1]))
    for r in range(rows):
        a = np.searchsorted(f, edges[r])
        b = max(a + 1, np.searchsorted(f, edges[r + 1]))
        out[r] = p[a:b].max(axis=0)
    return t, edges, db(out)


def chroma_of(m, sr):
    d = signal.resample_poly(m, 1, 4)
    sr = sr / 4
    f, t, z = signal.stft(d, sr, nperseg=4096, noverlap=3072)
    a = np.abs(z)
    k = (f > 90) & (f < 2500)
    f, a = f[k], a[k]
    peak = (a[1:-1] > a[:-2]) & (a[1:-1] >= a[2:]) & (a[1:-1] > a.max(axis=0) * 0.04)
    i, j = np.nonzero(peak)
    i += 1
    l, c, r = np.log(a[i - 1, j] + 1e-12), np.log(a[i, j] + 1e-12), np.log(a[i + 1, j] + 1e-12)
    fr = f[i] + (f[1] - f[0]) * 0.5 * (l - r) / (l - 2 * c + r - 1e-12)
    return t[j], 69 + 12 * np.log2(np.maximum(fr, 1) / 440), np.sqrt(a[i, j])


def key_tune(pk, t0, t1):
    s = (pk[0] >= t0) & (pk[0] < t1)
    if s.sum() < 20:
        return '?', 0.0, 0.0
    n, w = pk[1][s], pk[2][s]
    # the fine pitch is read from the peaks over about 350 Hz, where the bins are narrow enough
    h = n > 65
    cents = float('nan')
    if h.sum() > 10:
        z = np.sum(w[h] * np.exp(2j * np.pi * n[h]))
        cents = np.angle(z) / (2 * np.pi) * 100
    tn = 0 if np.isnan(cents) else cents / 100
    c = np.bincount(np.round(n - tn).astype(int) % 12, weights=w, minlength=12)
    sc = []
    for mode, prof in (('', MAJ), ('m', MIN)):
        for k in range(12):
            sc.append((np.corrcoef(c, np.roll(prof, k))[0, 1], NOTE[k] + mode))
    sc.sort(reverse=True)
    return sc[0][1] + '/' + sc[1][1], sc[0][0] - sc[1][0], cents


def onset_env(m, sr, hop=48):
    """Where energy starts: the rise over 8 ms of a 4 ms level, in three bands, every ms, lightly blurred."""
    total = 0
    for kind, hz, weight in (('low', 130, 1.5), ('bandpass', (200, 2000), 0.7), ('high', 3000, 1.0)):
        y = signal.sosfilt(signal.butter(2, hz, kind, fs=sr, output='sos'), m).astype('float64')
        c = np.cumsum(np.concatenate((np.zeros(4 * hop), y * y)))
        e = db((c[4 * hop::hop] - c[:-4 * hop:hop][:len(c[4 * hop::hop])]) / (4 * hop) + 1e-6)
        r = np.maximum(0, e[8:] - e[:-8])
        floor = np.clip((e[8:] - np.percentile(e, 97) + 40) / 40, 0, 1)
        total = total + weight * np.minimum(r, 18) * floor
    from scipy.ndimage import gaussian_filter1d
    # index k of the result is the instant (k + 4) ms: the middle of the rise
    return gaussian_filter1d(total, 4.0)


def comb(env, t0, p, i):
    k = np.rint((t0 + i * p) * 1000 - 4).astype(int)
    k = k[(k >= 0) & (k < len(env))]
    return env[k].sum() / max(1, len(i))


def grid(env, times, idx):
    """A straight beat grid: t = t0 + i * p, started at the tracker's beats and moved onto the attacks."""
    p, t0 = np.polyfit(idx, times, 1)
    keep = np.abs(times - (t0 + idx * p)) < 0.04
    if keep.sum() >= 4:
        p, t0 = np.polyfit(idx[keep], times[keep], 1)
    i = np.arange(idx.min(), idx.max() + 1)
    c = i.mean()
    mid = t0 + c * p
    best = (-1e9, p, mid)
    span = max(1, (i.max() - i.min()) / 2)
    dp = min(0.004 * p, 0.04 / span)
    for q in np.linspace(p - dp, p + dp, 33):
        for d in np.arange(-0.045, 0.0451, 0.002):
            v = comb(env, mid + d - c * q, q, i) * (1 - 0.1 * (abs(d) / 0.045) ** 2)
            if v > best[0]:
                best = (v, q, mid + d)
    v, q, md = best
    t0 = md - c * q
    off = np.mean([comb(env, t0 + f * q, q, i) for f in (0.15, 0.3, 0.4, 0.6, 0.7, 0.85)])
    # the blurred peak sits in the middle of the rise, and the attack starts some ms before it
    return t0 - 0.004, q, v / (off + 1e-9), np.median(np.abs(times - (t0 + idx * q)))


def clip(name, wav, beats, stems, out, png):
    x, sr = load(wav)
    m = x.mean(axis=1)
    dur = len(m) / sr
    b, chords, key = [], [], '?'
    for line in open(beats):
        w = line.split(None, 3)
        if w[0] == 'beat':
            b.append((float(w[1]), int(w[2])))
        elif w[0] == 'chord':
            chords.append((float(w[1]), float(w[2]), w[3].strip()))
        elif w[0] == 'key':
            key = line[4:].strip()
    k2 = (kweight(x, sr) ** 2).sum(axis=1)
    bands = [signal.sosfiltfilt(signal.butter(4, hz, kind, fs=sr, output='sos'), m) ** 2
             for hz, kind in ((150, 'low'), ((150, 2500), 'bandpass'), (2500, 'high'))]
    pre = [np.cumsum(np.concatenate(([0.0], v.astype('float64')))) for v in [k2] + bands]

    def mean(i, t0, t1):
        p, q = int(max(0, t0) * sr), int(min(dur, t1) * sr)
        return db((pre[i][q] - pre[i][p]) / max(1, q - p))

    voc = None
    if stems != '-':
        v, vsr = load(stems + '/vocals.wav')
        vk = (kweight(v, vsr) ** 2).sum(axis=1)
    else:
        v = None
    pk = chroma_of(m, sr)
    downs = [i for i, (t, p) in enumerate(b) if p == 1]
    lines = ['# %s dur %.3f madmom key %s lufs(ungated) %.1f' % (name, dur, key, mean(0, 0, dur) - 0.691)]
    if len(b) > 4:
        d = np.diff([t for t, _ in b])
        lines.append('# beat median %.4f s = %.2f BPM, beats a bar %d' % (np.median(d), 60 / np.median(d), max(p for _, p in b)))
    if len(b) > 8 and dur < 135:
        env = onset_env(m, sr)
        bt = np.array([t for t, _ in b])
        bi = np.arange(len(b))
        first = downs[0] if downs else 0
        n = max(1, int(np.ceil((dur - 3) / 30)))
        for part in [(0, dur)] + ([(dur * k / n, dur * (k + 1) / n) for k in range(n)] if n > 1 else [(0, dur / 2), (dur / 2, dur)]):
            g = (bt >= part[0]) & (bt < part[1])
            if g.sum() < 8:
                continue
            gt, gp, ratio, miss = grid(env, bt[g], bi[g])
            k = first + 4 * np.ceil((bi[g][0] - first) / 4)
            lines.append('# grid %3d-%3d: %.2f BPM beat %.5f bar line %.3f pulse x%.1f tracker miss %.0f ms' % (
                part[0], part[1], 60 / gp, gp, gt + k * gp, ratio, miss * 1000))
    tail = []
    for t0 in np.arange(max(0, dur - 2.0), dur, 0.25):
        tail.append('%.1f' % mean(0, t0, t0 + 0.25))
    lines.append('# last 2 s by 0.25 s: ' + ' '.join(tail))
    span = 6.0 if dur < 60 else 10.0
    keys = []
    for t0 in np.arange(0, dur - 2, span):
        k, c, ct = key_tune(pk, t0, t0 + span)
        keys.append('%d:%s(%.2f)%+.0fc' % (t0, k, c, ct))
    lines.append('# keys ' + ' '.join(keys))
    lines.append('bar\tatk\tmadmom\tbpm\tlufs\tlow\tmid\thi\tvox\tchords')
    at = []
    ends = [(b[i][0], i) for i in downs]
    for n, (t, i) in enumerate(ends):
        a = attack(m, sr, t)
        at.append(a)
        t2 = ends[n + 1][0] if n + 1 < len(ends) else dur
        j = ends[n + 1][1] if n + 1 < len(ends) else len(b) - 1
        bpm = 60 * (j - i) / (b[j][0] - t) if j > i else 0
        vx = ' '
        if v is not None:
            p, q = int(t * vsr), int(t2 * vsr)
            if q > p:
                vx = '%d' % max(-60, db(vk[p:q].mean()) - mean(0, t, t2))
        ch = []
        for s, e, l in chords:
            if min(e, t2) - max(s, t) > 0.3:
                ch.append(l.replace(':maj', '').replace(':min', 'm'))
        lines.append('%d\t%.3f\t%.2f\t%.1f\t%.1f\t%d\t%d\t%d\t%s\t%s' % (
            n + 1, a, t, bpm, mean(0, t, t2) - 0.691, mean(1, t, t2), mean(2, t, t2), mean(3, t, t2), vx, ' '.join(ch[:4])))
    open(out, 'w').write('\n'.join(lines) + '\n')

    row = 31.0 if dur <= 32 else 45.0
    rows = int(np.ceil(dur / row))
    fig, ax = plt.subplots(2 * rows, 1, figsize=(17, 4.2 * rows), gridspec_kw={'height_ratios': [4, 1] * rows}, squeeze=False)
    t, edges, s = logspec(m, sr, hop=1024)
    top = np.percentile(s, 99.9)
    hop = int(0.05 * sr)
    et = np.arange(0, len(m) - hop, hop) / sr
    cl = [db((p[hop + (et * sr).astype(int)] - p[(et * sr).astype(int)]) / hop) for p in (pre[0], pre[1])]
    for r in range(rows):
        A, B = ax[2 * r][0], ax[2 * r + 1][0]
        t0 = r * row
        g = (t >= t0) & (t < t0 + row)
        A.pcolormesh(t[g], edges[:-1], s[:, g], cmap='magma', vmin=top - 72, vmax=top, shading='nearest', rasterized=True)
        A.set_yscale('log')
        B.plot(et, cl[0], 'k', lw=.7)
        B.plot(et, cl[1], 'b', lw=.5)
        B.set_ylim(-45, 0)
        B.set_xticks(np.arange(t0, t0 + row, 1 if row < 40 else 2))
        for X in (A, B):
            X.set_xlim(t0, t0 + row)
        for n, a in enumerate(at):
            if t0 <= a < t0 + row:
                A.axvline(a, color='cyan', lw=.5, alpha=.6)
                A.text(a, 11000, str(n + 1), color='cyan', fontsize=9)
        if r == 0:
            A.set_title('%s  (cyan: bars; black: K-weighted level; blue: under 150 Hz)' % name)
    plt.tight_layout()
    plt.savefig(png, dpi=60)
    plt.close()


def look(wav, t0, t1, png, title=''):
    x, sr = load(wav, t0, t1)
    m = x.mean(axis=1)
    fig, ax = plt.subplots(2, 1, figsize=(15, 7), sharex=True, gridspec_kw={'height_ratios': [3, 1]})
    t, edges, s = logspec(m, sr, hop=384)
    top = np.percentile(s, 99.9)
    ax[0].pcolormesh(t + t0, edges[:-1], s, cmap='magma', vmin=top - 75, vmax=top, shading='nearest', rasterized=True)
    ax[0].set_yscale('log')
    ax[0].set_title(title or wav)
    ax[1].plot(np.arange(len(m)) / sr + t0, m, lw=.2)
    ax[1].set_xlim(t0, t1)
    plt.tight_layout()
    plt.savefig(png, dpi=65)


def sheet(out, paths):
    from PIL import Image
    ims = [Image.open(p).convert('RGB') for p in paths]
    w = max(i.width for i in ims)
    o = Image.new('RGB', (w, sum(i.height for i in ims)), 'white')
    y = 0
    for i in ims:
        o.paste(i, (0, y))
        y += i.height
    o.save(out)


# ---------------------------------------------------------------- seam review: thresholds
CLICK_WIN = 0.030      # s: where around a seam point a click is looked for
CLICK_REF = 2.0        # s each side: the neighbourhood whose own attacks the spike is compared with
CLICK_HP_HZ = 8000     # the residual a click is measured on
CLICK_RATIO = 1.5      # spike over the neighbourhood's 99.9th percentile (a -30 dBFS 1-sample step is far over it)
CLICK_LOCAL = 8.0      # spike over the rms of the 10 ms around it: a noise burst (drum) is a few sigma, a click is not
HOLE_BLOCK = 0.050     # s: level block
HOLE_HOP = 0.010
HOLE_DEPTH_DB = 25.0   # under the median of the 6 s around counts as hole
HOLE_RUN_MS = 120      # HOLE when the longest such run is over this
STEP_LU = 4.0          # 3 s before against 3 s after
LOW_DB = 6.0           # band steps (under 150 Hz, over 2500 Hz) after removing the overall step
HIGH_DB = 8.0
BEAT_MS = 25.0         # grid phase offset over this is BEAT
BEAT_PERIODIC = 0.2    # onset autocorrelation peak (normalised) a side needs to count as periodic
BEAT_SAME = 0.015      # two periods within this (after folding) share a grid
BEAT_FOLDS = ((1, 1), (2, 1), (1, 2), (3, 2), (2, 3))   # period B : period A, as m:n
ENV_RATE = 1000.0      # onset_env is one value a ms
SEAM_PAD = 10.5        # s of audio read each side of a seam
SEAM_PX = (10, 5.2)    # inches at 100 dpi: 1000 x 520
SONG_ROW = 180.0
STEM_CORR = 0.35       # rows of a stem (the timeline names the original)
HP_CORR, HP_LAG_MS = 0.8, 1.0
ENV_CORR, ENV_LAG_MS = 0.6, 60.0
VERIFY_MAX_SECONDS = 900.0
VERIFY_END_SLACK = 0.05


def read_tsv(path):
    rows = []
    with open(path) as f:
        head = f.readline().rstrip('\n').split('\t')
        for line in f:
            line = line.rstrip('\n')
            if not line.strip():
                continue
            v = line.split('\t')
            v += [''] * (len(head) - len(v))
            rows.append(dict(zip(head, v)))
    return rows


def mmss(t):
    return '%d:%04.1f' % (int(t // 60), t - 60 * (t // 60))


def cum(p):
    return np.concatenate(([0.0], np.cumsum(p.astype('float64'))))


def seg_db(c, i0, i1):
    """dB of the mean power between sample i0 and i1 of a cumulative sum; nan when under half a second."""
    i0, i1 = max(0, i0), min(len(c) - 1, i1)
    if i1 - i0 < SR // 2:
        return float('nan')
    return float(db((c[i1] - c[i0]) / (i1 - i0)))


def click_scan(x, sr, a0, pts):
    """For each (name, time): the largest spike of the 8 kHz-high-passed second difference within +-30 ms, over the
    99.9th percentile of the same signal over the 4 s around, 0 unless the spike is isolated (a click) and not a
    noise burst (a drum)."""
    hp = signal.sosfilt(signal.butter(4, CLICK_HP_HZ, 'high', fs=sr, output='sos'), x, axis=0)
    d2 = np.abs(np.diff(hp, 2, axis=0))
    d2[:int(0.02 * sr)] = 0
    w, rw, lw = int(CLICK_WIN * sr), int(CLICK_REF * sr), int(0.005 * sr)
    res = {}
    for name, p in pts:
        c = int(round((p - a0) * sr))
        i0, i1, r0, r1 = max(0, c - w), min(len(d2), c + w), max(0, c - rw), min(len(d2), c + rw)
        best = 0.0
        if i1 - i0 >= 8 and r1 - r0 >= 100:
            for ch in range(d2.shape[1]):
                seg = d2[i0:i1, ch]
                k = int(np.argmax(seg))
                pk, kk = seg[k], i0 + k
                near = np.concatenate((d2[max(0, kk - lw):max(0, kk - 4), ch], d2[kk + 5:kk + lw, ch]))
                loc = float(np.sqrt(np.mean(near ** 2))) if len(near) else 0.0
                if pk > CLICK_LOCAL * loc:
                    best = max(best, float(pk / max(np.percentile(d2[r0:r1, ch], 99.9), 1e-6)))
        res[name] = best
    return res


def hole_scan(ck, a0, sr, t):
    """(deepest 50 ms level in t+-1 against the median of t+-3 in dB, longest run more than 25 dB under it in ms)."""
    w, h = int(HOLE_BLOCK * sr), int(HOLE_HOP * sr)

    def levels(ta, tb):
        i0 = max(0, int(round((ta - a0) * sr)))
        i1 = min(len(ck) - 1, int(round((tb - a0) * sr)))
        st = np.arange(i0, i1 - w + 1, h)
        return db((ck[st + w] - ck[st]) / w)
    ref, zone = levels(t - 3, t + 3), levels(t - 1, t + 1)
    if len(ref) < 10 or len(zone) < 10:
        return float('nan'), 0.0
    med = float(np.median(ref))
    under = zone < med - HOLE_DEPTH_DB
    run = best = 0
    for u in under:
        run = run + 1 if u else 0
        best = max(best, run)
    return float(zone.min() - med), (best - 1) * HOLE_HOP * 1000 + HOLE_BLOCK * 1000 if best else 0.0


def period_of(e):
    """Beat period in s from the autocorrelation of an onset envelope (0.27-1.2 s), or None."""
    x = e - e.mean()
    n = len(x)
    f = np.fft.rfft(x, 2 * n)
    ac = np.fft.irfft(f * np.conj(f))[:n]
    if ac[0] <= 0 or n < 1500:
        return None
    ac = ac / ac[0]
    lo, hi = 270, min(1200, n - 2)
    k = lo + int(np.argmax(ac[lo:hi + 1]))
    if ac[k] < BEAT_PERIODIC:
        return None
    d = 0.0
    if lo < k < hi:
        den = ac[k - 1] - 2 * ac[k] + ac[k + 1]
        d = 0.5 * (ac[k - 1] - ac[k + 1]) / den if den < 0 else 0.0
    return (k + d) / ENV_RATE


def fold_profile(e, u, toward_end):
    """The envelope folded on a grid of period u that passes through the seam (the window's end for the side before,
    its start for the side after), weighting the half of the window next to the seam; one bin a ms, zero mean."""
    n = len(e)
    nb = max(8, int(round(u * ENV_RATE)))
    tk = np.arange(n) / ENV_RATE - (n / ENV_RATE if toward_end else 0.0)
    ramp = np.sin(0.5 * np.pi * np.arange(n) / n) ** 2
    w = ramp if toward_end else ramp[::-1]
    k = (np.floor(np.mod(tk, u) / u * nb).astype(int)) % nb
    p = np.bincount(k, weights=w * e, minlength=nb) / np.maximum(np.bincount(k, weights=w, minlength=nb), 1e-9)
    return p - p.mean()


def beat_offset(env, a0, t):
    """(bpm before, bpm after, offset in ms of the incoming beat grid against the outgoing one, or None): the shift
    between the two sides' envelopes folded on their own periods, both grids passing through the seam."""
    i = int(round((t - a0) * ENV_RATE))
    ea, eb = env[max(0, i - 5000):i], env[i:i + 5000]
    if len(ea) < 2000 or len(eb) < 2000:
        return None, None, None
    pa, pb = period_of(ea), period_of(eb)
    bpm = lambda p: None if p is None else 60 / p
    if pa is None or pb is None:
        return bpm(pa), bpm(pb), None
    m, n = min(BEAT_FOLDS, key=lambda f: abs(pb / pa / (f[0] / f[1]) - 1))
    if abs(pb / pa / (m / n) - 1) > BEAT_SAME:
        return bpm(pa), bpm(pb), None
    fa, fb = fold_profile(ea, pa / n, True), fold_profile(eb, pb / m, False)
    nb = min(len(fa), len(fb))
    fa, fb = signal.resample(fa, nb) if len(fa) != nb else fa, signal.resample(fb, nb) if len(fb) != nb else fb
    c = np.fft.irfft(np.conj(np.fft.rfft(fa)) * np.fft.rfft(fb), nb)
    s = int(np.argmax(c))
    return bpm(pa), bpm(pb), 1000 * (pa / n) * wrap(s / nb, 1.0)


def wrap(x, u):
    return (x + u / 2) % u - u / 2


def key_class(k):
    """Position on the circle of fifths of a key name from key_tune; relative major and minor are one place."""
    if not k or k[0] == '?':
        return None
    k = k.split('/')[0]
    minor = k.endswith('m')
    pc = NOTE.index(k[:-1] if minor else k)
    return ((pc + 3) % 12 if minor else pc) * 7 % 12


def seam_analysis(wav, info, row, nn):
    """Measure one seam; returns (table row dict, data for the picture)."""
    t, pe, st = float(row['time']), float(row['prev_end']), float(row['start'])
    dur = info.frames / info.samplerate
    x, sr = load(wav, t - SEAM_PAD, t + SEAM_PAD)
    a0 = max(0.0, t - SEAM_PAD)
    a0 = int(a0 * sr) / sr
    m = x.mean(axis=1)
    ix = lambda tt: int(round((tt - a0) * sr))
    kp = (kweight(x, sr) ** 2).sum(axis=1)
    ck = cum(kp)
    bands = []
    for hz, kind in ((150, 'low'), ((150, 2500), 'bandpass'), (2500, 'high')):
        y = signal.sosfilt(signal.butter(4, hz, kind, fs=sr, output='sos'), x, axis=0)
        bands.append(cum((y ** 2).sum(axis=1)))
    # click
    clk = click_scan(x, sr, a0, (('time', t), ('prev', pe), ('start', st)))
    cw = max(clk, key=clk.get)
    # hole
    depth, run = hole_scan(ck, a0, sr, t)
    # steps
    def step(c, secs):
        return seg_db(c, ix(t), ix(t + secs)) - seg_db(c, ix(t - secs), ix(t))
    s3, s10 = step(ck, 3), step(ck, 10)
    bs = [step(c, 3) - s3 for c in bands]
    # beat
    env = onset_env(m, sr)
    bpm_a, bpm_b, boff = beat_offset(env, a0, t)
    # key
    pk = chroma_of(m, sr)
    ka, _, _ = key_tune(pk, max(0, t - 5 - a0), t - a0)
    kb, _, _ = key_tune(pk, t - a0, t + 5 - a0)
    ca, cb = key_class(ka), key_class(kb)
    fif = None if ca is None or cb is None else min(abs(ca - cb), 12 - abs(ca - cb))
    flags = []
    if clk[cw] > CLICK_RATIO:
        flags.append('CLICK')
    if run > HOLE_RUN_MS:
        flags.append('HOLE')
    if abs(s3) > STEP_LU:
        flags.append('STEP')
    if abs(bs[0]) > LOW_DB:
        flags.append('LOW')
    if abs(bs[2]) > HIGH_DB:
        flags.append('HIGH')
    if boff is not None and abs(boff) > BEAT_MS:
        flags.append('BEAT')
    f1 = lambda v, fmt='%.1f': '-' if v is None or v != v else fmt % v
    out = {
        'nn': '%02d' % nn, 'time': '%.3f' % t, 'mmss': mmss(t), 'role': row['role'] or '-', 'join_in': row['join_in'] or '-',
        'click': '%.1f' % clk[cw] if clk[cw] > 0 else '0', 'click_at': cw if clk[cw] > 0 else '-',
        'hole_db': f1(depth), 'hole_ms': '%.0f' % run, 'step3': f1(s3), 'step10': f1(s10),
        'low': f1(bs[0]), 'mid': f1(bs[1]), 'high': f1(bs[2]),
        'bpm_a': f1(bpm_a), 'bpm_b': f1(bpm_b), 'beat_ms': f1(boff, '%+.1f'),
        'key_a': ka.split('/')[0] if ka[0] != '?' else '-', 'key_b': kb.split('/')[0] if kb[0] != '?' else '-',
        'fifths': f1(fif, '%d'), 'flags': ','.join(flags) or '-'}
    return out, dict(x=x, sr=sr, a0=a0, m=m, ck=ck, bands=bands, t=t, pe=pe, st=st, dur=dur)


SEAM_COLS = ['nn', 'time', 'mmss', 'role', 'join_in', 'click', 'click_at', 'hole_db', 'hole_ms', 'step3', 'step10',
             'low', 'mid', 'high', 'bpm_a', 'bpm_b', 'beat_ms', 'key_a', 'key_b', 'fifths', 'flags']


def seam_picture(png, name, out, d):
    x, sr, a0, m, ck, t = d['x'], d['sr'], d['a0'], d['m'], d['ck'], d['t']
    fig = plt.figure(figsize=SEAM_PX, dpi=100)
    gs = fig.add_gridspec(3, 1, height_ratios=[3, 1.3, 1.3], hspace=0.38, left=0.07, right=0.93, top=0.93, bottom=0.07)
    A, B, C = fig.add_subplot(gs[0]), fig.add_subplot(gs[1]), fig.add_subplot(gs[2])
    i0, i1 = max(0, int((t - 6 - a0) * sr)), min(len(m), int((t + 6 - a0) * sr))
    ts, edges, s = logspec(m[i0:i1], sr, hop=512)
    top = np.percentile(s, 99.9)
    A.pcolormesh(ts + a0 + i0 / sr, edges[:-1], s, cmap='magma', vmin=top - 75, vmax=top, shading='nearest', rasterized=True)
    A.set_yscale('log')
    A.set_ylim(30, 16000)
    A.set_yticks([50, 100, 200, 500, 1000, 2000, 5000, 10000])
    A.set_yticklabels(['50', '100', '200', '500', '1k', '2k', '5k', '10k'])
    A.axvline(t, color='white', lw=0.8, ls='--')
    A.vlines(d['pe'], 30, 55, color='cyan', lw=1.5)
    A.vlines(d['st'], 30, 55, color='lime', lw=1.5)
    A.set_title('%s s%s %s %s / %s / %s' % (name, out['nn'], out['mmss'], out['role'], out['join_in'], out['flags']), fontsize=9)
    A.set_xlim(t - 6, t + 6)
    # waveform with the levels
    nb = (i1 - i0) // 192
    wv = m[i0:i0 + nb * 192].reshape(nb, 192)
    tb = a0 + (i0 + 96 + np.arange(nb) * 192) / sr
    B.fill_between(tb, wv.min(axis=1), wv.max(axis=1), color='0.55', lw=0)
    B.set_ylim(-max(0.05, np.abs(m[i0:i1]).max() * 1.05), max(0.05, np.abs(m[i0:i1]).max() * 1.05))
    B.set_xlim(t - 6, t + 6)
    B.axvline(t, color='k', lw=0.6, ls='--')
    B.tick_params(labelsize=7)
    B2 = B.twinx()
    w, h = int(HOLE_BLOCK * sr), int(HOLE_HOP * sr) * 5
    st_ = np.arange(i0, i1 - w, h)
    B2.plot(a0 + (st_ + w // 2) / sr, db((ck[st_ + w] - ck[st_]) / w) - 0.691, 'k', lw=0.8, label='K 50 ms')
    B2.plot(a0 + (st_ + w // 2) / sr, db((d['bands'][0][st_ + w] - d['bands'][0][st_]) / w), 'b', lw=0.8, label='under 150 Hz')
    B2.set_ylim(-80, 0)
    B2.tick_params(labelsize=7)
    B2.set_ylabel('dB', fontsize=7)
    B2.legend(loc='lower left', fontsize=6, ncol=2, frameon=False)
    # zoom
    j0, j1 = max(0, int((t - 0.15 - a0) * sr)), min(len(m), int((t + 0.15 - a0) * sr))
    tz = a0 + np.arange(j0, j1) / sr
    C.plot(tz, x[j0:j1, 0], lw=0.5, color='tab:blue')
    if x.shape[1] > 1:
        C.plot(tz, x[j0:j1, 1], lw=0.5, color='tab:orange')
    C.axvline(t, color='k', lw=0.8, ls='--')
    for q, col in ((d['pe'], 'cyan'), (d['st'], 'lime')):
        if t - 0.15 <= q <= t + 0.15:
            C.axvline(q, color=col, lw=1.0)
    C.set_xlim(t - 0.15, t + 0.15)
    C.tick_params(labelsize=7)
    fig.savefig(png, dpi=100)
    plt.close(fig)


def seams(wav, seams_tsv, timeline_tsv, outdir, name):
    import os
    from PIL import Image
    os.makedirs(outdir, exist_ok=True)
    info = sf.info(wav)
    rows = read_tsv(seams_tsv)
    table, pngs = [], []
    for n, row in enumerate(rows, 1):
        out, d = seam_analysis(wav, info, row, n)
        p = '%s/%s_s%02d.png' % (outdir, name, n)
        seam_picture(p, name, out, d)
        table.append(out)
        pngs.append(p)
    text = '\t'.join(SEAM_COLS) + '\n' + ''.join('\t'.join(r[c] for c in SEAM_COLS) + '\n' for r in table)
    open('%s/%s_seams.tsv' % (outdir, name), 'w').write(text)
    sys.stdout.write(text)
    for k in range(0, len(pngs), 4):
        ims = [Image.open(p).convert('RGB') for p in pngs[k:k + 4]]
        w, h = ims[0].size
        o = Image.new('RGB', (2 * w, 2 * h), 'white')
        for i, im in enumerate(ims):
            o.paste(im, ((i % 2) * w, (i // 2) * h))
        o.save('%s/%s_seamsheet_%d.png' % (outdir, name, k // 4 + 1))


# ---------------------------------------------------------------- verify
def decode_src(path):
    r = subprocess.run(['ffmpeg', '-v', 'error', '-nostdin', '-i', path, '-f', 'f32le', '-ar', str(SR), '-ac', '2', '-'],
                       capture_output=True)
    if r.returncode or not r.stdout:
        raise RuntimeError(r.stderr.decode(errors='replace').strip() or 'no samples')
    return np.frombuffer(r.stdout, dtype='<f4').reshape(-1, 2)


def src_slice(src, t0, n):
    """n samples of src from time t0 (zeros outside), mono."""
    a = int(round(t0 * SR))
    o = np.zeros(n)
    lo, hi = max(a, 0), min(a + n, len(src))
    if hi > lo:
        o[lo - a:hi - a] = src[lo:hi].mean(axis=1)
    return o


def hp300(v):
    return signal.sosfilt(signal.butter(4, 300, 'high', fs=SR, output='sos'), v)


def windows_of(r, rows, wlen=0.5):
    lo, hi = r['a'] + 0.5, r['b'] - 0.5 - wlen
    if hi < lo:
        lo, hi = r['a'], r['b'] - wlen
    free = [c for c in np.arange(lo, hi + 1e-9, 0.25)
            if not any(o is not r and o['a'] < c + wlen and o['b'] > c for o in rows)]
    if not free:
        return []
    idx = sorted(set(int(round(i)) for i in np.linspace(0, len(free) - 1, min(3, len(free)))))
    return [free[i] for i in idx]


def lag_corr(wav, r, src, tau, wlen=0.5, maxlag=0.05):
    """Normalised correlation and lag (ms) of the 300 Hz-high-passed output window at tau against the source."""
    n, lg = int(wlen * SR), int(maxlag * SR)
    pad = int(0.2 * SR)
    o, _ = load(wav, tau - 0.2, tau + wlen + 0.01)   # whole samples: without the margin the window can come out one short
    o = hp300(o.mean(axis=1))[pad:pad + n]
    s0 = r['in'] + (tau - r['a']) * r['rate']
    b = hp300(src_slice(src, s0 - 0.2 - maxlag, pad + n + 2 * lg))[pad:]
    if len(o) < n or len(b) < n + 2 * lg or o.std() == 0:
        return 0.0, 0.0
    dots = signal.fftconvolve(b, o[::-1], 'valid')
    c2 = np.concatenate(([0.0], np.cumsum(b * b)))
    en = c2[n:n + 2 * lg + 1] - c2[:2 * lg + 1]
    cc = dots / (np.sqrt(en * np.sum(o * o)) + 1e-12)
    k = int(np.argmax(cc))
    return float(cc[k]), (k - lg) / SR * 1000


def env_corr(wav, r, src):
    """50 ms level envelopes over the middle of the row, time mapped by the rate; (corr, lag ms)."""
    dur = r['b'] - r['a']
    span = min(0.6 * dur, 12.0)
    ta = r['a'] + (dur - span) / 2
    hop, blk = 0.01, 0.05
    nk = int((span - blk) / hop)
    o, _ = load(wav, ta, ta + span)
    co = cum(o.mean(axis=1) ** 2)
    w = int(blk * SR)
    st = (np.arange(nk) * hop * SR).astype(int)
    lo = db((co[np.minimum(st + w, len(co) - 1)] - co[np.minimum(st, len(co) - 1)]) / w)
    s_ta = r['in'] + (ta - r['a']) * r['rate']
    reg0 = s_ta - 0.1
    sm = src_slice(src, reg0, int((span * r['rate'] + 0.3) * SR))
    cs = cum(sm ** 2)
    best = (-2.0, 0.0)
    for j in range(-6, 7):
        t0 = (0.1 + np.arange(nk) * hop * r["rate"] + j * hop * r['rate']) * SR
        a = np.clip(t0.astype(int), 0, len(cs) - 1)
        bq = np.clip((t0 + blk * r['rate'] * SR).astype(int), 0, len(cs) - 1)
        ls = db((cs[bq] - cs[a]) / np.maximum(1, bq - a))
        x, y = np.maximum(lo, lo.max() - 50), np.maximum(ls, ls.max() - 50)
        c = 0.0 if x.std() == 0 or y.std() == 0 else float(np.corrcoef(x, y)[0, 1])
        if c > best[0]:
            best = (c, j * hop * 1000)
    return best


def verify(wav, timeline):
    info = sf.info(wav)
    fdur = info.frames / info.samplerate
    rows = []
    for t in read_tsv(timeline):
        rows.append(dict(idx=t['idx'], a=float(t['song_start']), b=float(t['song_end']), path=t['src_path'],
                         **{'in': float(t['src_in'])}, out=float(t['src_out']), rate=float(t['rate']),
                         semi=float(t['semitones']), role=t['role']))
    cache, bad, npass, nhid, nfail = {}, [], 0, 0, 0
    for r in rows:
        stem = r['role'].startswith('stem')
        name = '%s idx %s %s rate %.4g semi %.3g %s' % ('%s', r['idx'], r['role'] or '-', r['rate'], r['semi'], r['path'].split('/')[-1])
        if r['b'] - r['a'] < 0.7:
            nhid += 1
            bad.append(name % 'hidden' + '  (shorter than 0.7 s)')
            continue
        plain = abs(r['rate'] - 1) < 1e-9 and abs(r['semi']) < 1e-9
        wins = windows_of(r, rows) if plain else ([0] if windows_of(r, rows) else [])
        if not wins:
            nhid += 1
            bad.append(name % 'hidden' + '  (wholly overlapped)')
            continue
        if r['path'] not in cache:
            try:
                cache[r['path']] = decode_src(r['path'])
            except Exception as e:
                cache[r['path']] = None
                bad.append('FAIL cannot decode %s: %s' % (r['path'], e))
        src = cache[r['path']]
        if src is None:
            nfail += 1
            continue
        if plain:
            res = [lag_corr(wav, r, src, tau) for tau in wins]
            thr, lagmax = (STEM_CORR if stem else HP_CORR), HP_LAG_MS
            ok = any(c >= thr and abs(l) <= lagmax for c, l in res)
            c, l = max(res, key=lambda v: v[0])
        else:
            c, l = env_corr(wav, r, src)
            thr, lagmax = (STEM_CORR if stem else ENV_CORR), ENV_LAG_MS
            ok = c >= thr and abs(l) <= lagmax
        if ok:
            npass += 1
        else:
            nfail += 1
            bad.append(name % 'FAIL' + '  corr %.2f lag %+.1f ms (need %.2f within %.0f ms)' % (c, l, thr, lagmax))
    if rows:
        last = max(r['b'] for r in rows)
        if last > fdur + VERIFY_END_SLACK:
            nfail += 1
            bad.append('FAIL last row ends %.3f s, after the file ends %.3f s' % (last, fdur))
    if fdur > VERIFY_MAX_SECONDS:
        nfail += 1
        bad.append('FAIL file is %.3f s, over %.1f s' % (fdur, VERIFY_MAX_SECONDS))
    for b in bad:
        print(b)
    print('verify: %d rows, %d pass, %d hidden, %d FAIL' % (len(rows), npass, nhid, nfail))
    return 1 if nfail else 0


# ---------------------------------------------------------------- song
def song(wav, seams_tsv, png, title):
    from matplotlib.ticker import FuncFormatter
    info = sf.info(wav)
    dur = info.frames / info.samplerate
    st = [float(r['time']) for r in read_tsv(seams_tsv)]
    rows = int(np.ceil(dur / SONG_ROW))
    fig, ax = plt.subplots(2 * rows, 1, figsize=(14, 2.6 * rows), gridspec_kw={'height_ratios': [3, 1] * rows}, squeeze=False)
    fig.subplots_adjust(left=0.04, right=0.99, top=0.95 if rows > 1 else 0.88, bottom=0.04, hspace=0.35)
    top = None
    for r in range(rows):
        A, B = ax[2 * r][0], ax[2 * r + 1][0]
        t0, t1 = r * SONG_ROW, min(dur, (r + 1) * SONG_ROW)
        x, sr = load(wav, t0, t1)
        ts, edges, s = logspec(x.mean(axis=1), sr, hop=2400, rows=160)
        if top is None:
            top = np.percentile(s, 99.9)
        A.pcolormesh(ts + t0, edges[:-1], s, cmap='magma', vmin=top - 72, vmax=top, shading='nearest', rasterized=True)
        A.set_yscale('log')
        A.set_ylim(30, 16000)
        A.set_yticks([100, 1000, 10000])
        A.set_yticklabels(['100', '1k', '10k'], fontsize=7)
        c = cum((kweight(x, sr) ** 2).sum(axis=1))
        nb = int((t1 - t0) // 1)
        b0 = (np.arange(nb) * sr).astype(int)
        B.plot(t0 + np.arange(nb) + 0.5, db((c[b0 + sr] - c[b0]) / sr) - 0.691, 'k', lw=0.7)
        B.set_ylim(-45, 0)
        B.tick_params(labelsize=7)
        for X in (A, B):
            X.set_xlim(t0, t0 + SONG_ROW)
            X.set_xticks(np.arange(t0, t0 + SONG_ROW + 1, 15))
            X.xaxis.set_major_formatter(FuncFormatter(lambda v, _: '%d:%02d' % (v // 60, v % 60)))
        A.set_xticklabels([])
        for n, q in enumerate(st, 1):
            if t0 <= q < t0 + SONG_ROW:
                A.axvline(q, color='cyan', lw=0.5, alpha=0.7)
                A.text(q, 11000, '%02d' % n, color='cyan', fontsize=7)
                B.axvline(q, color='0.5', lw=0.5)
        if r == 0:
            A.set_title('%s  (cyan: seams; black: K-weighted 1 s level, dB)' % title, fontsize=10)
    fig.savefig(png, dpi=100)
    plt.close(fig)


if __name__ == '__main__':
    cmd, a = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else ('', [])
    if cmd == 'clip' and len(a) == 6:
        clip(*a)
    elif cmd == 'look' and len(a) >= 4:
        look(a[0], float(a[1]), float(a[2]), a[3], a[4] if len(a) > 4 else '')
    elif cmd == 'sheet' and len(a) >= 2:
        sheet(a[0], a[1:])
    elif cmd == 'seams' and len(a) == 5:
        seams(*a)
    elif cmd == 'verify' and len(a) == 2:
        sys.exit(verify(*a))
    elif cmd == 'song' and len(a) == 4:
        song(*a)
    else:
        sys.exit(__doc__)
