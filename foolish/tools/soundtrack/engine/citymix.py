#!/usr/bin/env python3
"""citymix.py: the measuring and drawing half of the city mixes (splice.c renders).

  clip  id wav beats.txt stems_dir|- out.txt out.png
        one recording: a table with a row per bar (attack time of the downbeat, tempo, level, band levels, vocal
        stem level, chroma key, tuning, chord) and a picture with the numbered bar lines.
  look  wav t0 t1 out.png [title]
        a spectrogram and waveform of any stretch.
  sheet out.png in.png ...
        pictures stacked into one.

Tables and pictures only; nothing here is a listening verdict."""
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


def grid(m, sr, beats):
    """A straight beat grid through the beat attacks: time of beat i = t0 + i * p, plus how well they sit on it."""
    at = np.array([attack(m, sr, t, 0.06) for t, _ in beats])
    i = np.arange(len(at))
    use = np.ones(len(at), bool)
    p, t0 = np.polyfit(i, at, 1)
    for cut in (0.08, 0.05, 0.03, 0.02, 0.015, 0.012):
        e = at - (t0 + i * p)
        u = np.abs(e - np.median(e[use])) < cut
        if u.sum() < max(6, len(at) // 5):
            break
        use = u
        p, t0 = np.polyfit(i[use], at[use], 1)
    return t0, p, np.median(np.abs(at - (t0 + i * p))), use.mean()


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
        gt, gp, ge, gu = grid(m, sr, b)
        g1 = gt + downs[0] * gp if downs else gt
        lines.append('# grid %.3f BPM, beat %.5f s, first bar line %.3f, median miss %.0f ms, %d%% of beats on it' % (
            60 / gp, gp, g1, ge * 1000, gu * 100))
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


if __name__ == '__main__':
    cmd, a = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else ('', [])
    if cmd == 'clip' and len(a) == 6:
        clip(*a)
    elif cmd == 'look' and len(a) >= 4:
        look(a[0], float(a[1]), float(a[2]), a[3], a[4] if len(a) > 4 else '')
    elif cmd == 'sheet' and len(a) >= 2:
        sheet(a[0], a[1:])
    else:
        sys.exit(__doc__)
