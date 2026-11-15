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
from scipy.spatial.distance import pdist

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare as C

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
    if a.distance <= 0 or ns == 1:
        lab = np.arange(ns)
    else:
        Z = hierarchy.linkage(pdist(F, 'cosine'), 'complete')
        lab = hierarchy.fcluster(Z, a.distance, 'distance') - 1
    # clusters ordered by first appearance
    order = {}
    for l in lab:
        order.setdefault(int(l), len(order))
    cl = np.array([order[int(l)] for l in lab])
    ncl = len(order)
    base = 24 if ncl <= 100 else 0
    if base + ncl - 1 > 127 and not a.split_roots:
        raise SystemExit('extract: %d clusters need roots up to %d (MIDI maximum 127); raise --distance or pass --split-roots' % (ncl, base + ncl - 1))
    os.makedirs(a.samples_dir, exist_ok=True)
    man, ex_of, stored = [], {}, []
    for c in range(ncl):
        mem = [i for i in range(ns) if cl[i] == c]
        mean = F[mem].mean(axis=0)
        mean /= max(np.linalg.norm(mean), 1e-12)
        best = max(segs[i]['n'] for i in mem)
        cand = [i for i in mem if segs[i]['n'] == best]
        ex = min(cand, key=lambda i: (1 - float(F[i] @ mean), i))
        ex_of[c] = ex
        wav = fade(segs[ex]['seg'], sr)
        stored.append(wav)
        fn = os.path.join(a.samples_dir, '%s_%03d.wav' % (a.name, c))
        write_wav(fn, wav)
        man.append('%d 0 0 %s' % (base + c % 128 if base == 0 else base + c, manifest_path(fn)))
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
    # events and velocities
    rr = np.zeros(ns)
    for i, s in enumerate(segs):
        n = min(s['n'] - s['v0'], int(VEL_S * sr))
        e = segs[ex_of[cl[i]]]
        den = rms(stored[cl[i]][s['e0']:s['e0'] + n])
        rr[i] = rms(s['seg'][s['v0']:s['v0'] + n]) / max(den, 1e-12)
    rmax = float(rr.max())
    vel = np.clip(rr / rmax - 0.05, 0, 0.95)
    gain_db = 20 * np.log10(rmax)
    ev = []
    for i, s in enumerate(segs):
        beat = s['ev0'] / sr * a.bpm / 60.0
        ln = s['len'] / sr * a.bpm / 60.0
        ev.append('%s,%d,%s,%s' % (fmt4(beat), base + cl[i], fmt4(ln), fmt4(vel[i])))
    with open(a.events, 'w') as f:
        f.write('# %s gain_db %.4f\n' % (a.name, gain_db))
        for k in range(0, len(ev), 40):
            f.write('EVENTS %s: %s\n' % (a.name, ' '.join(ev[k:k + 40])))
    with open(a.table, 'w') as f:
        f.write('start_s\tlength_s\tcluster\texemplar\tr\tv\tdist_to_exemplar\tseg_start_s\tseg_end_s\n')   # the last two: the cut as made, in the clip's time
        for i, s in enumerate(segs):
            d = 1 - float(F[i] @ F[ex_of[cl[i]]])
            f.write('%.4f\t%.4f\t%d\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\n' % (s['ev0'] / sr, s['len'] / sr, cl[i],
                    'yes' if ex_of[cl[i]] == i else 'no', rr[i], vel[i], max(d, 0.0), a.start + s['a0'] / sr, a.start + s['a1'] / sr))
    tot = sum(len(w) for w in stored) / sr
    print('%s D=%g: segments %d, clusters %d, exemplar audio %.3f s, stored share %.4f of %.3f s (part gain %.2f dB)'
          % (a.name, a.distance, ns, ncl, tot, tot / win_s, win_s, gain_db))
    return dict(segments=ns, clusters=ncl, exemplar_s=tot, share=tot / win_s, gain_db=gain_db)


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
    ap.add_argument('--lead-ms', type=float, default=6.0)
    ap.add_argument('--min-seg-ms', type=float, default=50.0)
    return extract(ap.parse_args(argv))


if __name__ == '__main__':
    main()
