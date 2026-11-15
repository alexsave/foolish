# extract.py - cut one-shot samples and exact-timing events for one stem of a window of a recording.
#
# What it does: reads a stem wav (plus optional wavs summed into it), cuts the window, finds onsets with compare.py's detector
#   (never quantised), cuts the stem into one segment per onset, clusters the segments by their spectral shape (complete linkage
#   at a cosine distance D), stores the LONGEST member of each cluster as a one-shot wav, and writes one event per segment
#   (exact start in beats, exact length, velocity) so the engine's play.c (sampler, exact mode) re-plays the window.
# Inputs: --stem WAV [--add WAV ...] --name PART --start S --end S --bpm B --distance D --samples-dir DIR --manifest PATH
#   --events PATH --table PATH [--lead-ms 6] [--min-seg-ms 50] [--split-roots] [--timing trim|attack|segment] [--feat-eps 0.01]
# Outputs: PART_NNN.wav in --samples-dir (native rate, stereo, float32, raw level, 2 ms fade in, 8 ms fade out); the manifest
#   ("root 0 0 path", root = 24 + cluster index, or 0 + index above 100 clusters, error above 127); the events file (first line
#   "# PART gain_db X", then "EVENTS PART: beat,pitch,len,vel ..." lines of at most 40 events); the table (tsv).
# How to run: python3 -I extract.py --stem drums.wav --name DRUMS --start 0 --end 42 --bpm 160 --distance 0.1 ...
# Choices the brief left open, all visible as options: the feature's compression (--feat-eps 0.01: log1p(magnitude / (the segment's
#   peak magnitude * eps)), so it does not depend on level and has 40 dB of range); merged onsets keep the stronger; --timing: 'trim'
#   (default) starts each event at its segment start plus what the engine's smp_trim drops from the front of the exemplar's stored
#   file (everything before the first sample at -50 dB of the peak, less 1.5 ms), so the sound lands where the segment's own sound
#   was; 'segment' is the literal segment start (the sound then lands early by that trim); 'attack' also finds each segment's attack
#   by one rule (first 1 ms block at 20 percent of the loudest block of the first 80 ms) and lands attacks, not segment starts, on
#   the original's. --split-roots allows more than 128 clusters: manifest k (MANIFEST, MANIFEST.2, ...) holds clusters 128k..128k+127
#   and events carry pitch 128k + root (play.c routes them).
# Pitch (iteration 2): --roots spaced stores sample i at root 100 * i + 50 (an event's pitch is root + shift in semitones, fractional
#   allowed; no 128 limit, no manifest splitting; the events file's first line then ends "roots spaced" and assemble.py writes the
#   parts.txt keyword `roots spaced`, so play.c does not route pitch >= 127.5). --transpose f0 (monophonic stem): per segment the median f0
#   (compare.py's yin, 30-400 Hz by default, --f0-range), the voiced share and f0 at 25/50/75 percent; features are compared after
#   shifting each by its f0 (log-frequency bins, 36 per octave), an event's shift = 12 log2(f0 member / f0 exemplar) limited to
#   +-max-shift (default 12; a member beyond it is split into its own cluster); unvoiced segments (voiced share < 0.5) cluster only
#   among themselves at shift 0; the exemplar is the longest member with voiced share >= 0.8. --transpose shift (polyphonic): distance =
#   the minimum over integer k in +-max-shift (default 5) semitones of 1 - A . shift(B, k) (zero padded, so symmetric; k = 0 wins ties
#   within 1e-6); a member's shift is the k that matches the exemplar shifted up by k. --merge-sustain TAU removes an onset when the
#   feature of the 80 ms before it and of the 80 ms after it are closer than TAU and the 20 ms RMS does not rise by more than 3 dB.
#   Velocity: RMS over the common length of member and exemplar (the exemplar's transposed length); --vel-window-ms N keeps the
#   old fixed window (100 = the old rule).
# Known failure modes: the detector fires some ms before or after the true attack depending on the sound; a stationary sustained
#   sound is cut at spurious onsets into fragments that replay with the wrong envelope; a cluster's members are all played as its
#   exemplar at the exemplar's pitch (no transposition), so members of another pitch come out wrong; more than 128 clusters need
#   --split-roots.
# Randomness: none. Everything is deterministic.
import argparse, os, sys
import numpy as np
from scipy import signal
from scipy.io import wavfile
from scipy.cluster import hierarchy
from scipy.spatial.distance import pdist, squareform

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare as C

F0_REF = 100.0               # --transpose f0 aligns every feature to this f0
YW, YHOP = 1024, 512          # compare.yin's frame and hop (22050 Hz samples)
FEAT_MAX_S = 0.25            # the feature looks at the first 250 ms of a segment
VEL_S = 0.1                  # the velocity ratio looks at the first 100 ms
FADE_IN_S, FADE_OUT_S = 0.002, 0.008
SILENCE_DB = -60.0           # a segment this far below the stem's peak (RMS) is dropped
PRE_DB = -50.0               # sound above this before the first onset makes a segment from the window start
BINS_PER_OCT, FMIN, FMAX = 36, 40.0, 12000.0
NFFT, WIN, HOP = 8192, 2048, 512
FEAT_EPS = 0.01             # the feature is log1p(magnitude / (the segment's peak magnitude * FEAT_EPS)); 0 = log1p(1e4 * magnitude), which is level
                            # dependent and so compressed that D = 0.35 merges a whole stem into one cluster


def read_wav(path):
    sr, x = wavfile.read(path)
    if x.dtype == np.int16:
        x = x / 32768.0
    elif x.dtype == np.int32:
        x = x / 2147483648.0
    elif x.dtype == np.uint8:
        x = (x - 128) / 128.0
    x = x.astype(np.float64)
    if x.ndim == 1:
        x = np.stack([x, x], axis=1)
    return sr, x


def write_wav(path, x):
    wavfile.write(path, SR_OUT[0], x.astype(np.float32))


SR_OUT = [44100]


def merge_onsets(t, w, min_gap):
    """Onsets closer than min_gap to the last kept one are merged: the stronger of the two stays (ties: the earlier)."""
    kt, kw = [], []
    for ti, wi in zip(t, w):
        if kt and ti - kt[-1] < min_gap:
            if wi > kw[-1]:
                kt[-1], kw[-1] = ti, wi
        else:
            kt.append(ti); kw.append(wi)
    return np.array(kt), np.array(kw)


def feature(seg, sr):
    """Log-compressed magnitude on a log-frequency axis (36 bins per octave, 40 Hz to 12 kHz), mean over the first
    250 ms (or the whole segment when shorter), unit norm."""
    x = seg[:int(FEAT_MAX_S * sr)].mean(axis=1)
    n = len(x)
    nfr = max(1, 1 + (n - 1) // HOP)
    xp = np.concatenate([x, np.zeros(WIN)])
    win = np.hanning(WIN)
    frames = np.stack([xp[i * HOP:i * HOP + WIN] * win for i in range(nfr)])
    mag = np.abs(np.fft.rfft(frames, NFFT, axis=1)) * (2.0 / win.sum())
    fr = np.fft.rfftfreq(NFFT, 1.0 / sr)
    nb = int(np.floor(np.log2(FMAX / FMIN) * BINS_PER_OCT)) + 1
    cf = FMIN * 2.0 ** (np.arange(nb) / BINS_PER_OCT)
    cf = cf[cf < sr / 2]
    lm = np.stack([np.interp(cf, fr, m) for m in mag])
    if FEAT_EPS:
        lm = lm / max(float(lm.max()), 1e-12) / FEAT_EPS     # relative to the segment's own peak, so the feature does not depend on its level
    else:
        lm = 1e4 * lm
    f = np.log1p(lm).mean(axis=0)
    return f / max(np.linalg.norm(f), 1e-12)


def fade(x, sr):
    y = x.copy()
    n = len(y)
    a = min(int(round(FADE_IN_S * sr)), n // 2)
    b = min(int(round(FADE_OUT_S * sr)), n // 2)
    if a > 0:
        y[:a] *= (0.5 - 0.5 * np.cos(np.pi * np.arange(a) / a))[:, None]
    if b > 0:
        y[n - b:] *= (0.5 + 0.5 * np.cos(np.pi * np.arange(1, b + 1) / b))[:, None]
    return y


def engine_trim(x, sr):
    """The samples sampler.h's smp_trim drops from the front of a stored sample: everything before the first sample whose left or
    right value reaches 0.3 percent of the peak (-50 dB), less 1.5 ms. Reproduced here so an event can start where the engine's
    sound really starts (a sample with quiet pre-roll would otherwise land early by the pre-roll)."""
    pk = max(float(np.max(np.abs(x))), 1e-9)
    hit = np.nonzero(np.max(np.abs(x), axis=1) >= pk * 0.003)[0]
    a = int(hit[0]) if len(hit) else len(x)
    return max(0, a - int(0.0015 * sr))


def attack_index(seg, sr):
    """Where a segment's sound starts: the first 1 ms block whose RMS reaches 20 percent (-14 dB) of the loudest 1 ms block in the
    segment's first 80 ms. The same rule for every segment, so two members of a cluster agree on what their attack is whatever
    pre-roll or tail of the previous sound they carry."""
    b = max(1, int(round(0.001 * sr)))
    x = seg[:int(0.08 * sr)].mean(axis=1) if len(seg) else seg
    nb = len(x) // b
    if nb < 1:
        return 0
    e = np.sqrt(np.mean(x[:nb * b].reshape(nb, b) ** 2, axis=1))
    hit = np.nonzero(e >= 0.2 * e.max())[0]
    return int(hit[0]) * b if len(hit) else 0


def rms(x):
    return float(np.sqrt(np.mean(np.square(x)))) if x.size else 0.0


def manifest_path(p):
    """sampler.h re-roots a path holding /soundtrack/samples/ onto $SAMPLES; a file under .../soundtrack-samples/ is
    written that way, any other file by its absolute path."""
    p = os.path.abspath(p)
    key = '/soundtrack-samples/'
    if key in p:
        return '/soundtrack/samples/' + p.split(key, 1)[1]
    return p


def fmt4(v):
    return '%.4f' % v


def drop_continuations(x, sr, t, w, tau):
    """Removes an onset when the sound simply continues across it: cosine distance between the feature of the 80 ms before and the 80 ms
    after is under tau, and the 20 ms RMS does not rise by more than 3 dB. Each boundary is judged on the original signal."""
    n80, n20 = int(round(0.08 * sr)), int(round(0.02 * sr))
    keep = []
    for ti in t:
        i = int(round(ti * sr))
        pre, post = x[max(0, i - n80):i], x[i:i + n80]
        if len(pre) < n20 or len(post) < n20:
            keep.append(True); continue
        d = 1 - float(feature(pre, sr) @ feature(post, sr))
        rise = 20 * np.log10(max(rms(x[i:i + n20]), 1e-12) / max(rms(x[i - n20:i]), 1e-12))
        keep.append(not (d < tau and rise <= 3.0))
    keep = np.array(keep, dtype=bool)
    return t[keep], w[keep], int((~keep).sum())


def f0_stats(segs, sr, fmin, fmax):
    """Per segment: median f0 of the voiced frames (compare.py's yin and its voicing rule), voiced share, f0 at 25/50/75 percent."""
    ys = []
    for g in segs:
        x = C.resample(g['seg'].mean(axis=1), sr)
        f0, per, en = C.yin(x, C.SR, fmin, fmax)
        nv = min(len(f0), max(1, 1 + (len(x) - YW) // YHOP))
        ys.append((f0[:nv], per[:nv], en[:nv]))
    enmax = max(float(e.max()) for _, _, e in ys)
    for g, (f0, per, en) in zip(segs, ys):
        v = (per > 0.75) & (en > 10 ** (-45 / 20) * max(enmax, 1e-12))
        g['voiced'] = float(v.mean())
        g['f0'] = float(np.median(f0[v])) if v.any() else float('nan')
        g['f0p'] = []
        for pc in (0.25, 0.5, 0.75):
            k = int(round(pc * (len(f0) - 1)))
            idx = [j for j in sorted(range(len(f0)), key=lambda j: (abs(j - k), j)) if v[j] and abs(j - k) <= 2]
            g['f0p'].append(float(f0[idx[0]]) if idx else float('nan'))
        g['isv'] = g['voiced'] >= 0.5 and g['f0'] == g['f0']


def align_feature(f, bins):
    """The feature with its content moved down by `bins` (a segment of f0 f gets bins = 36 log2(f / ref): its f0 lands on ref)."""
    b = np.arange(len(f))
    g = np.interp(b + bins, b, f, left=0.0, right=0.0)
    return g / max(float(np.linalg.norm(g)), 1e-12)


def shift_bins(F, k):
    """Features with their content moved UP by k semitones (3 bins each), zero padded."""
    n = k * (BINS_PER_OCT // 12)
    out = np.zeros_like(F)
    if n > 0:
        out[:, n:] = F[:, :-n]
    elif n < 0:
        out[:, :n] = F[:, -n:]
    else:
        out[:] = F
    return out


def shift_distances(F, maxk):
    """M[a, b] = min over k of 1 - F[a] . shift_up(F[b], k) and K[a, b] = that k (k = 0 wins ties within 1e-6, then the smaller |k|)."""
    M = 1 - F @ F.T
    K = np.zeros(M.shape, dtype=int)
    for k in sorted([j for j in range(-maxk, maxk + 1) if j], key=lambda j: (abs(j), -j)):
        Dk = 1 - F @ shift_bins(F, k).T
        better = Dk < M - 1e-6
        M = np.where(better, Dk, M); K = np.where(better, k, K)
    return M, K


def extract(a):
    global FEAT_EPS
    FEAT_EPS = a.feat_eps
    sr, x = read_wav(a.stem)
    for extra in a.add or []:
        sr2, y = read_wav(extra)
        if sr2 != sr:
            raise SystemExit('extract: %s is %d Hz, the stem is %d Hz' % (extra, sr2, sr))
        n = min(len(x), len(y))
        x = x[:n] + y[:n]
    SR_OUT[0] = sr
    i0, i1 = int(round(a.start * sr)), min(len(x), int(round(a.end * sr)))
    x = x[i0:i1]
    nwin = len(x)
    win_s = nwin / sr
    peak = float(np.max(np.abs(x)))
    if peak <= 0:
        raise SystemExit('extract: the window is silent')
    mono = x.mean(axis=1)
    t, w, _ = C.detect_onsets(C.resample(mono, sr))
    t, w = merge_onsets(t, w, a.min_seg_ms / 1000.0)
    nmerged = 0
    if a.merge_sustain is not None:
        t, w, nmerged = drop_continuations(x, sr, t, w, a.merge_sustain)
        print('merge-sustain %g: removed %d boundaries, %d kept' % (a.merge_sustain, nmerged, len(t)))
    lead = a.lead_ms / 1000.0
    starts = [max(0.0, ti - lead) for ti in t]
    bounds = []                     # (start s, end s)
    if len(t) == 0:
        bounds = [(0.0, win_s)]
    else:
        pre_end = int(round(starts[0] * sr))
        if starts[0] > 0 and pre_end > 0 and np.max(np.abs(mono[:pre_end])) > peak * 10 ** (PRE_DB / 20):
            bounds.append((0.0, starts[0]))
        for k, s in enumerate(starts):
            bounds.append((s, starts[k + 1] if k + 1 < len(starts) else win_s))
    segs = []
    for s, e in bounds:
        a0, a1 = int(round(s * sr)), min(nwin, int(round(e * sr)))
        if a1 - a0 < 2:
            continue
        seg = x[a0:a1]
        if 20 * np.log10(max(rms(seg), 1e-12) / peak) < SILENCE_DB:
            continue
        segs.append(dict(start=a0 / sr, a0=a0, a1=a1, seg=seg))
    ns = len(segs)
    if ns == 0:
        raise SystemExit('extract: no segments')
    for g in segs:
        g['tr'] = engine_trim(fade(g['seg'], sr), sr)     # what the engine drops from the front of this segment's stored file
        g['att'] = attack_index(g['seg'], sr)             # where its sound really starts, inside the segment
        g['n'] = g['a1'] - g['a0']
    F = np.stack([feature(s['seg'], sr) for s in segs])
    mode = a.transpose
    if mode == 'f0':
        f0_stats(segs, sr, a.f0_range[0], a.f0_range[1])
        FA = np.stack([align_feature(F[i], BINS_PER_OCT * np.log2(g['f0'] / F0_REF)) if g['isv'] else F[i] for i, g in enumerate(segs)])
    else:
        FA = F
    M = K = None
    if ns > 1:
        if mode == 'shift':
            M, K = shift_distances(F, int(round(a.max_shift)) if a.max_shift is not None else 5)
            M = (M + M.T) / 2; np.fill_diagonal(M, 0.0)
            cond = squareform(np.maximum(M, 0.0), checks=False)
        else:
            cond = pdist(FA, 'cosine')
            if mode == 'f0':
                uv = np.array([not g['isv'] for g in segs])
                cond = cond + 10.0 * squareform((uv[:, None] != uv[None, :]).astype(float), checks=False)
    if a.distance <= 0 or ns == 1:
        lab = np.arange(ns)
    else:
        Z = hierarchy.linkage(cond, 'complete')
        lab = hierarchy.fcluster(Z, a.distance, 'distance') - 1
    maxs = a.max_shift if a.max_shift is not None else 12
    def pick_exemplar(mem):
        pool = mem
        if mode == 'f0':
            hi = [i for i in mem if segs[i]['voiced'] >= 0.8 and segs[i]['isv']]
            pool = hi or mem
        best = max(segs[i]['n'] for i in pool)
        cand = [i for i in pool if segs[i]['n'] == best]
        mean = FA[mem].mean(axis=0); mean /= max(np.linalg.norm(mean), 1e-12)
        return min(cand, key=lambda i: (1 - float(FA[i] @ mean), i))
    nsplit = 0
    if mode == 'f0':      # a member further than max-shift from its cluster's exemplar becomes a cluster of its own kind
        lab = lab.copy(); nxt = int(lab.max()) + 1
        queue = [[i for i in range(ns) if lab[i] == c] for c in range(nxt)]
        while queue:
            mem = queue.pop(0)
            e = pick_exemplar(mem)
            if not segs[e]['isv']:
                continue
            off = [i for i in mem if segs[i]['isv'] and abs(12 * np.log2(segs[i]['f0'] / segs[e]['f0'])) > maxs + 1e-9]
            if off:
                for i in off:
                    lab[i] = nxt
                nxt += 1; nsplit += len(off); queue.append(off)
    # clusters ordered by first appearance
    order = {}
    for l in lab:
        order.setdefault(int(l), len(order))
    cl = np.array([order[int(l)] for l in lab])
    ncl = len(order)
    spaced = a.roots == 'spaced'
    base = 24 if ncl <= 100 else 0
    if not spaced and base + ncl - 1 > 127 and not a.split_roots:
        raise SystemExit('extract: %d clusters need roots up to %d (MIDI maximum 127); raise --distance or pass --split-roots or --roots spaced' % (ncl, base + ncl - 1))
    os.makedirs(a.samples_dir, exist_ok=True)
    man, ex_of, stored = [], {}, []
    for c in range(ncl):
        mem = [i for i in range(ns) if cl[i] == c]
        ex = pick_exemplar(mem)
        ex_of[c] = ex
        wav = fade(segs[ex]['seg'], sr)
        stored.append(wav)
        fn = os.path.join(a.samples_dir, '%s_%03d.wav' % (a.name, c))
        write_wav(fn, wav)
        root = 100 * c + 50 if spaced else (base + c % 128 if base == 0 else base + c)
        man.append('%d 0 0 %s' % (root, manifest_path(fn)))
    shifts = np.zeros(ns)
    for i, g in enumerate(segs):
        e = ex_of[cl[i]]
        if i == e:
            continue
        if mode == 'f0' and g['isv'] and segs[e]['isv']:
            shifts[i] = 12 * np.log2(g['f0'] / segs[e]['f0'])
        elif mode == 'shift':
            shifts[i] = K[i, e]
    shifts = np.round(shifts, 4)
    if spaced:
        with open(a.manifest, 'w') as f:
            f.write('\n'.join(man) + '\n')
    else:
        # --split-roots (more than 128 clusters): manifest k (MANIFEST, MANIFEST.2, ...) holds clusters 128k..128k+127 with roots 0..127,
        # and the events carry pitch 128k + root; play.c sends pitch >= 128 to the k-th manifest of the part
        for k in range((len(man) + 127) // 128):
            with open(a.manifest if k == 0 else '%s.%d' % (a.manifest, k + 1), 'w') as f:
                f.write('\n'.join(man[128 * k:128 * k + 128]) + '\n')
    # placement: the engine plays the exemplar from its trimmed start (tr samples in). 'trim': event start = member start + the
    # exemplar's tr. 'attack': event start = member start + (member attack - exemplar attack) + the exemplar's tr, length = what
    # is left of the member after its attack + the exemplar's pre-attack that plays. 'segment': the segment start and length as cut.
    for i, g in enumerate(segs):
        e = segs[ex_of[cl[i]]]
        if a.timing == 'attack':
            g['ev0'] = g['a0'] + g['att'] - e['att'] + e['tr']
            g['len'] = (g['n'] - g['att']) + (e['att'] - e['tr'])
            g['v0'], g['e0'] = g['att'], e['att']          # velocity windows start at the attacks
        elif a.timing == 'trim':
            g['ev0'] = g['a0'] + e['tr']                   # the engine starts the exemplar tr in: start the event tr later
            g['len'] = g['n'] - e['tr']
            g['v0'] = g['e0'] = e['tr']
        else:
            g['ev0'], g['len'] = g['a0'], g['n']
            g['v0'], g['e0'] = 0, 0
    # events and velocities: the member and the exemplar as played (transposed: shorter or longer by the ratio) over their common length
    rr = np.zeros(ns); short_ms = np.zeros(ns)
    for i, s in enumerate(segs):
        ratio = 2.0 ** (shifts[i] / 12.0)
        st = stored[cl[i]]
        n = min(s['n'] - s['v0'], int((len(st) - s['e0']) / ratio))
        if a.vel_window_ms is not None:
            n = min(n, int(a.vel_window_ms / 1000.0 * sr))
        n = max(n, 1)
        den = rms(st[s['e0']:s['e0'] + int(round(n * ratio))])
        rr[i] = rms(s['seg'][s['v0']:s['v0'] + n]) / max(den, 1e-12)
        short_ms[i] = max(0.0, (s['len'] - (len(st) - s['e0']) / ratio) / sr * 1000.0)
    rmax = float(rr.max())
    vel = np.clip(rr / rmax - 0.05, 0, 0.95)
    gain_db = 20 * np.log10(rmax)
    ev = []
    for i, s in enumerate(segs):
        beat = s['ev0'] / sr * a.bpm / 60.0
        ln = s['len'] / sr * a.bpm / 60.0
        root = 100 * cl[i] + 50 if spaced else base + cl[i]
        ev.append('%s,%s,%s,%s' % (fmt4(beat), fmt4(root + shifts[i]) if spaced else '%d' % root if shifts[i] == 0 else fmt4(root + shifts[i]), fmt4(ln), fmt4(vel[i])))
    with open(a.events, 'w') as f:
        f.write('# %s gain_db %.4f%s\n' % (a.name, gain_db, ' roots spaced' if spaced else ''))
        for k in range(0, len(ev), 40):
            f.write('EVENTS %s: %s\n' % (a.name, ' '.join(ev[k:k + 40])))
    with open(a.table, 'w') as f:
        f.write('start_s\tlength_s\tcluster\texemplar\tr\tv\tdist_to_exemplar\tseg_start_s\tseg_end_s\tf0\tshift\tvoiced\tf0_25\tf0_50\tf0_75\tshort_ms\n')   # seg_*: the cut as made, in the clip's time
        for i, s in enumerate(segs):
            e = ex_of[cl[i]]
            if M is not None and mode == 'shift':
                d = float(M[i, e])
            else:
                d = 1 - float(FA[i] @ FA[e])
            fv = lambda v: '%.3f' % v if v == v else 'nan'
            f0c = [fv(s['f0']), '%.4f' % s['voiced']] + [fv(v) for v in s['f0p']] if mode == 'f0' else ['-', '-', '-', '-', '-']
            f.write('%.4f\t%.4f\t%d\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%s\t%.4f\t%s\t%s\t%s\t%s\t%.1f\n' % (s['ev0'] / sr, s['len'] / sr, cl[i],
                    'yes' if e == i else 'no', rr[i], vel[i], max(d, 0.0), a.start + s['a0'] / sr, a.start + s['a1'] / sr,
                    f0c[0], shifts[i], f0c[1], f0c[2], f0c[3], f0c[4], short_ms[i]))
    tot = sum(len(w) for w in stored) / sr
    nshort = int((short_ms > 30).sum())
    glides = [abs(1200 * np.log2(g['f0p'][2] / g['f0p'][0])) for g in segs if mode == 'f0' and g['isv'] and g['f0p'][0] == g['f0p'][0] and g['f0p'][2] == g['f0p'][2]]
    print('%s D=%g: segments %d, clusters %d, exemplar audio %.3f s, stored share %.4f of %.3f s (part gain %.2f dB)'
          % (a.name, a.distance, ns, ncl, tot, tot / win_s, win_s, gain_db))
    print('STATS merged=%d median_abs_shift=%.4f short30=%d split=%d median_glide_cents=%s unvoiced=%d'
          % (nmerged, float(np.median(np.abs(shifts))), nshort, nsplit, '%.1f' % float(np.median(glides)) if glides else 'n/a',
             sum(1 for g in segs if mode == 'f0' and not g['isv'])))
    return dict(segments=ns, clusters=ncl, exemplar_s=tot, share=tot / win_s, gain_db=gain_db, merged=nmerged, short30=nshort)


def main(argv=None):
    ap = argparse.ArgumentParser(description='Cut one-shot samples and exact events for one stem.')
    ap.add_argument('--stem', required=True)
    ap.add_argument('--add', nargs='+', action='extend')
    ap.add_argument('--name', required=True)
    ap.add_argument('--start', type=float, required=True)
    ap.add_argument('--end', type=float, required=True)
    ap.add_argument('--bpm', type=float, required=True)
    ap.add_argument('--distance', type=float, required=True)
    ap.add_argument('--samples-dir', required=True)
    ap.add_argument('--manifest', required=True)
    ap.add_argument('--events', required=True)
    ap.add_argument('--table', required=True)
    ap.add_argument('--split-roots', action='store_true', help='allow more than 128 clusters (extra manifests, pitch 128k + root)')
    ap.add_argument('--timing', choices=['trim', 'attack', 'segment'], default='trim', help="'trim': segment start, moved by what the engine's smp_trim drops from the exemplar; 'attack': lands each attack where the original's was; 'segment': the segment start as cut")
    ap.add_argument('--feat-eps', type=float, default=FEAT_EPS, help='feature compression: log1p(magnitude / (segment peak * eps)); 0 = log1p(1e4 * magnitude)')
    ap.add_argument('--roots', choices=['midi', 'spaced'], default='midi', help='spaced: sample i has root 100 i + 50 and an event pitch is root + shift')
    ap.add_argument('--transpose', choices=['none', 'f0', 'shift'], default='none', help='f0: monophonic stem, shift = f0 ratio; shift: polyphonic, best integer semitone shift')
    ap.add_argument('--max-shift', type=float, default=None, help='largest shift in semitones (default 12 for f0, 5 for shift)')
    ap.add_argument('--f0-range', type=float, nargs=2, default=[30.0, 400.0], metavar=('FMIN', 'FMAX'))
    ap.add_argument('--merge-sustain', type=float, default=None, metavar='TAU', help='remove an onset where the sound simply continues (feature distance under TAU, RMS rise at most 3 dB)')
    ap.add_argument('--vel-window-ms', type=float, default=None, help='velocity window from the start (default: the whole common length; 100 = the iteration 1 rule)')
    ap.add_argument('--lead-ms', type=float, default=6.0)
    ap.add_argument('--min-seg-ms', type=float, default=50.0)
    return extract(ap.parse_args(argv))


if __name__ == '__main__':
    main()
