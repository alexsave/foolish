# bass_notes.py - notes of a monophonic stem (a bass) from its f0 track, plus a small sample set cut from its cleanest notes.
#
# What it does: reads a stem wav, cuts the window, runs compare.py's yin (30-200 Hz, hop about 10 ms at 22050 Hz), and turns the
#   voiced frames into notes. Voiced = periodicity above 0.75 and the frame within 40 dB of the stem's loudest frame. A run of
#   voiced frames is joined across unvoiced gaps shorter than 40 ms. Inside a run, a note is the frames whose f0 stays within
#   60 cents of the note's running median; a new note starts when the pitch leaves that band for 30 ms or more, when the 20 ms
#   RMS rises by more than 6 dB within 40 ms (a re-attack on the same pitch, split at the trough before the rise, never closer
#   than 50 ms to the note's start), or after an unvoiced gap of 40 ms or more. A note shorter than 60 ms joins the contiguous
#   neighbour nearest in pitch when that is within 100 cents, else it is dropped and counted. The onset is refined to the level
#   rise: the first 1 ms step of the causal 10 ms RMS within 12 dB of the note's peak level, searched from 40 ms before the
#   pitch run starts (20 ms before it when the previous note is contiguous, 0 before it for a re-attack: the previous note's tail
#   would otherwise count as the rise). Per note: onset, length, median f0 as a fractional MIDI pitch, glide (f0 at 10 percent
#   versus 90 percent of the note, cents, negative = falling), RMS over the note's first 100 ms.
# Inputs: --stem WAV --name PART --start S --end S --bpm B --table TSV --events PATH [--keep-cents] [--f0-range LO HI]
#   [--cut-samples N DIR MANIFEST --source-clip NAME --samples-log TSV [--rel-samples]]
# Outputs: the table (tsv, one row per note, clip time), the events file ("# PART gain_db X" then "EVENTS PART: beat,pitch,len,vel"
#   lines, beat = (onset - start) * bpm / 60 with 4 decimals, pitch the fractional MIDI rounded to the nearest semitone unless
#   --keep-cents, vel = clip(r / r_max - 0.05, 0, 0.95), gain_db = 20 log10 r_max, r = the note's RMS, so the part gain is in
#   stem units; --rel-samples divides each note's r by the RMS of the cut sample the sampler would pick for it, so a sampler part
#   plays at the level of the note whatever the level of its source note), and with --cut-samples the N longest clean notes
#   (glide under 30 cents, no re-attack inside, level not falling more than 12 dB over the note, at most one per 4 semitones),
#   each from 6 ms before its onset to its end plus up to 300 ms of tail (never into the next onset), pitch-corrected to its
#   nearest semitone by one constant resampling, not normalised, 2 ms fade in, 30 ms fade out; the manifest holds "root 0 0 path",
#   root = the note's MIDI number; the samples log is a tsv with source clip, stem, window and note.
# How to run: python3 -I bass_notes.py --stem bass.wav --name BASS --start 0 --end 42 --bpm 160 --table n.tsv --events ev.txt
# Known failure modes: yin octave errors become short notes of another pitch; the 46 ms yin window makes a note's pitch frames
#   start 10-40 ms after its true onset (the level refinement undoes this where the level rises, not on a legato pitch change);
#   a note that swells by more than 6 dB inside 40 ms after its first 50 ms is split.
# Randomness: none.
import argparse, os, sys
from fractions import Fraction
import numpy as np
from scipy import signal
from scipy.io import wavfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare as C

SR = C.SR                      # 22050
HOP = 220                      # yin hop: 9.98 ms
W = 1024                       # yin window
PER_MIN = 0.75
LEVEL_DB = -40.0               # voiced frames are within this of the stem's loudest frame
BAND_CENTS = 60.0              # pitch band around a note's running median
OUT_S = 0.030                  # leaving the band this long starts a new note
GAP_S = 0.040                  # an unvoiced gap this long ends a note
REATTACK_DB = 6.0              # 20 ms RMS rise within REATTACK_WIN_S
REATTACK_WIN_S = 0.040
REATTACK_MIN_S = 0.050         # a re-attack split is never closer than this to the note's start
MIN_NOTE_S = 0.060
JOIN_CENTS = 100.0
REFINE = True                  # refine the onset to the level rise
REFINE_DB = 12.0
REFINE_BACK_S = 0.040
REFINE_BACK_LEGATO_S = 0.020
CLEAN_GLIDE_CENTS = 30.0
CLEAN_FALL_DB = 12.0
TAIL_S, PRE_S = 0.300, 0.006
FADE_IN_S, FADE_OUT_S = 0.002, 0.030
HOP_S = HOP / SR
ENV_HOP = 22                   # 1 ms level grid at 22050 Hz


def read_wav(path):
    sr, x = wavfile.read(path)
    if x.dtype == np.int16:
        x = x / 32768.0
    elif x.dtype == np.int32:
        x = x / 2147483648.0
    x = x.astype(np.float64)
    if x.ndim == 1:
        x = np.stack([x, x], axis=1)
    return sr, x


def midi(f):
    return 69 + 12 * np.log2(f / 440.0)


def causal_rms_db(x, win, hop):
    """dB of the RMS over the `win` samples ending at each multiple of hop (index k = time k * hop / sr)."""
    c = np.concatenate([[0.0], np.cumsum(x ** 2)])
    idx = np.arange(0, len(x) + 1, hop)
    a = np.maximum(idx - win, 0)
    return 10 * np.log10(np.maximum((c[idx] - c[a]) / np.maximum(idx - a, 1), 1e-20))


class Note:
    pass


def analyze(mono, sr, fmin=30.0, fmax=200.0):
    """mono at sr (any rate). Returns (notes, stats dict). Times are seconds from the start of `mono`."""
    x = C.resample(mono, sr)
    f0, per, en = C.yin(x, SR, fmin, fmax, W=W, hop=HOP)
    tc = (np.arange(len(f0)) * HOP + W / 2) / SR
    voiced = (per > PER_MIN) & (en > 10 ** (LEVEL_DB / 20) * max(en.max(), 1e-12))
    m = midi(np.maximum(f0, 1e-3))
    e10 = causal_rms_db(x, int(0.010 * SR), ENV_HOP)
    e20 = causal_rms_db(x, int(0.020 * SR), ENV_HOP)
    et = np.arange(len(e10)) * ENV_HOP / SR
    peak_db = e20.max()
    nv = int(voiced.sum())
    # 1. runs of voiced frames, joined across unvoiced gaps shorter than GAP_S
    vi = np.nonzero(voiced)[0]
    runs = []
    for i in vi:
        if runs and (i - runs[-1][-1] - 1) * HOP_S < GAP_S:
            runs[-1].append(i)
        else:
            runs.append([i])
    # 2. split each run by pitch band
    segs = []                                   # dicts: frames, reattack
    for run in runs:
        cur, pend = [run[0]], []
        done = []
        for f in run[1:]:
            med = np.median(m[cur])
            if abs(m[f] - med) * 100 <= BAND_CENTS:
                cur += pend + [f]; pend = []
            else:
                pend.append(f)
                if (tc[pend[-1]] - tc[pend[0]]) + HOP_S >= OUT_S:
                    done.append(cur); cur, pend = pend, []
        cur += pend
        done.append(cur)
        for fr in done:
            segs.append(dict(frames=fr, reattack=False))
    # 3. re-attack splits (a trough followed by a rise of more than REATTACK_DB within REATTACK_WIN_S)
    nwin = int(round(REATTACK_WIN_S * SR / ENV_HOP))
    out = []
    for s in segs:
        fr = s['frames']
        t0, t1 = tc[fr[0]], tc[fr[-1]]
        k0 = int(np.searchsorted(et, t0 + REATTACK_MIN_S)); k1 = int(np.searchsorted(et, t1))
        cuts = []
        k = k0
        while k < k1:
            lo, hi = max(0, k - nwin), min(len(e20), k + nwin + 1)
            if e20[k] <= e20[lo:hi].min() + 1e-9 and e20[k:k + nwin + 1].max() - e20[k] > REATTACK_DB and e20[k:k + nwin + 1].max() > peak_db + LEVEL_DB:
                cuts.append(et[k]); k += int(round(REATTACK_MIN_S * SR / ENV_HOP))
            else:
                k += 1
        bounds = [t0 - 1] + cuts + [t1 + 1]
        for j in range(len(bounds) - 1):
            ff = [f for f in fr if bounds[j] <= tc[f] < bounds[j + 1]]
            if ff:
                out.append(dict(frames=ff, reattack=(j > 0), cut=bounds[j] if j > 0 else None))
    segs = out

    def span(s):
        fr = s['frames']
        return tc[fr[0]], tc[fr[-1]] + W / 2 / SR           # raw start (first pitch frame), raw end (end of the last window)

    def refine(s, prev):
        t0, t1 = span(s)
        if s['reattack']:
            lo = s['cut']
        elif prev is not None and t0 - prev['end'] <= GAP_S:
            lo = t0 - REFINE_BACK_LEGATO_S
        else:
            lo = t0 - REFINE_BACK_S
        if prev is not None:
            lo = max(lo, prev['onset'] + 0.030)
        lo = max(lo, 0.0)
        a, b = int(np.searchsorted(et, max(lo, t0 - 0.02))), int(np.searchsorted(et, min(t1, t0 + 0.15)))
        pk = e10[a:max(b, a + 1)].max()
        k = int(np.searchsorted(et, lo))
        while k < len(e10) and e10[k] < pk - REFINE_DB:
            k += 1
        return float(et[min(k, len(et) - 1)]) if REFINE else float(t0)

    def build(segs):
        prev = None
        for s in segs:
            t0, t1 = span(s)
            s['start_raw'], s['end'] = t0, t1
            s['onset'] = refine(s, prev)
            s['dur'] = t1 - s['onset']
            s['pitch'] = float(np.median(m[s['frames']]))
            prev = s

    build(segs)
    # 4. notes shorter than MIN_NOTE_S: join the contiguous neighbour nearest in pitch within JOIN_CENTS, else drop
    dropped = joined = 0
    dropped_at = []
    changed = True
    while changed:
        changed = False
        for i, s in enumerate(segs):
            if s['dur'] >= MIN_NOTE_S:
                continue
            cand = []
            for j in (i - 1, i + 1):
                if 0 <= j < len(segs):
                    n = segs[j]
                    gap = s['start_raw'] - n['end'] if j < i else n['start_raw'] - s['end']
                    if gap <= GAP_S and abs(n['pitch'] - s['pitch']) * 100 <= JOIN_CENTS:
                        cand.append((abs(n['pitch'] - s['pitch']), j))
            if cand and MIN_NOTE_S > 0:
                j = min(cand)[1]
                segs[j]['frames'] = sorted(segs[j]['frames'] + s['frames'])
                del segs[i]; joined += 1
            else:
                dropped_at.append(s['onset']); del segs[i]; dropped += 1
            build(segs)
            changed = True
            break
    # 5. note records
    notes = []
    for i, s in enumerate(segs):
        n = Note()
        n.onset = s['onset']
        nxt = segs[i + 1]['onset'] if i + 1 < len(segs) else None
        n.end = s['end'] if nxt is None else min(s['end'], nxt)
        n.reattack = s['reattack']
        fr = s['frames']
        n.f0 = float(np.median(f0[fr]))
        n.midi = float(np.median(m[fr]))
        k = len(fr)
        def at(p):
            c = int(round(p * (k - 1)))
            return float(np.median(f0[fr[max(0, c - 1):c + 2]]))
        n.glide = float(1200 * np.log2(at(0.9) / at(0.1))) if k >= 3 else 0.0
        n.frames = fr
        notes.append(n)
    stats = dict(voiced_frames=nv, dropped=dropped, dropped_at=dropped_at, joined=joined, e20=e20, e20_t=et, tc=tc, voiced=voiced)
    covered = 0
    for n in notes:
        covered += int(((tc >= n.onset) & (tc < n.end) & voiced).sum())
    stats['covered'] = covered / nv if nv else float('nan')
    return notes, stats


def note_level(x44, sr, n, e20=None):
    a = int(round(n.onset * sr)); b = int(round(min(n.end, n.onset + 0.1) * sr))
    seg = x44[a:max(b, a + 1)]
    return float(np.sqrt(np.mean(seg ** 2))) if len(seg) else 0.0


def clean(n, stats, next_onset):
    """glide under CLEAN_GLIDE_CENTS, no re-attack inside (the notes are already split at re-attacks: no rise of more than 3 dB
    after the first 50 ms), the 20 ms level not falling more than CLEAN_FALL_DB between the note's peak and its end."""
    if abs(n.glide) >= CLEAN_GLIDE_CENTS:
        return False
    et, e20 = stats['e20_t'], stats['e20']
    a, b = int(np.searchsorted(et, n.onset)), int(np.searchsorted(et, n.end))
    seg = e20[a:b]
    if len(seg) < 10:
        return False
    if seg.max() - seg[-1] > CLEAN_FALL_DB:
        return False
    k50 = min(len(seg) - 1, int(0.05 / (ENV_HOP / SR)))
    rest = seg[k50:]
    if len(rest) > 1 and (rest - np.minimum.accumulate(rest)).max() > 3.0:
        return False
    return True


def cut_samples(x, sr, notes, stats, N, outdir, manifest, name, log, source, win0, win1, stem):
    rows = []
    order = sorted(range(len(notes)), key=lambda i: -(notes[i].end - notes[i].onset))
    best = {}
    for i in order:
        n = notes[i]
        nxt = notes[i + 1].onset if i + 1 < len(notes) else None
        if not clean(n, stats, nxt):
            continue
        b = int(np.floor(round(n.midi) / 4))
        if b not in best:
            best[b] = i
    pick = sorted(sorted(best.values(), key=lambda i: -(notes[i].end - notes[i].onset))[:N], key=lambda i: notes[i].midi)
    os.makedirs(outdir, exist_ok=True)
    man = []
    for j, i in enumerate(pick):
        n = notes[i]
        nxt = notes[i + 1].onset if i + 1 < len(notes) else len(x) / sr
        tail = max(0.0, min(TAIL_S, nxt - 0.003 - n.end))
        a = max(0, int(round((n.onset - PRE_S) * sr))); b = min(len(x), int(round((n.end + tail) * sr)))
        seg = x[a:b]
        root = int(round(n.midi))
        ratio = 2.0 ** ((root - n.midi) / 12.0)                       # one constant resampling to the nearest semitone
        fr = Fraction(ratio).limit_denominator(2000)
        seg = np.stack([signal.resample_poly(seg[:, c], fr.denominator, fr.numerator) for c in range(seg.shape[1])], axis=1)
        na, nb = min(int(FADE_IN_S * sr), len(seg) // 2), min(int(FADE_OUT_S * sr), len(seg) // 2)
        seg = seg.copy()
        seg[:na] *= (0.5 - 0.5 * np.cos(np.pi * np.arange(na) / max(na, 1)))[:, None]
        seg[len(seg) - nb:] *= (0.5 + 0.5 * np.cos(np.pi * np.arange(1, nb + 1) / max(nb, 1)))[:, None]
        fn = os.path.join(outdir, '%s_%02d_m%d.wav' % (name, j, root))
        wavfile.write(fn, sr, seg.astype(np.float32))
        man.append('%d 0 0 %s' % (root, manifest_path(fn)))
        r = float(np.sqrt(np.mean(seg[:int(0.1 * sr)] ** 2)))
        rows.append((fn, source, stem, win0, win1, n.onset + win0, n.end - n.onset, n.midi, root, n.glide, r, ratio))
    with open(manifest, 'w') as f:
        f.write('\n'.join(man) + '\n')
    with open(log, 'w') as f:
        f.write('file\tsource_clip\tstem\twindow_start_s\twindow_end_s\tnote_onset_s\tnote_len_s\tnote_midi\troot\tglide_cents\tsample_rms\tpitch_correction_ratio\n')
        for r in rows:
            f.write('%s\t%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.3f\t%d\t%.1f\t%.6f\t%.5f\n' % r)
    return rows


def manifest_path(p):
    p = os.path.abspath(p)
    key = '/soundtrack-samples/'
    if key in p:
        return '/soundtrack/samples/' + p.split(key, 1)[1]
    return p


def events_text(notes, levels, name, win0, bpm, keep_cents, relsamp=None):
    r = np.array(levels, dtype=float)
    if relsamp is not None:
        r = np.array([lv / max(relsamp(n), 1e-12) for n, lv in zip(notes, levels)])
    rmax = float(r.max()) if len(r) else 1.0
    vel = np.clip(r / rmax - 0.05, 0, 0.95)
    ev = []
    for n, v in zip(notes, vel):
        pitch = n.midi if keep_cents else round(n.midi)
        ev.append('%.4f,%s,%.4f,%.4f' % ((n.onset) * bpm / 60.0, ('%.4f' % pitch) if keep_cents else '%d' % pitch, (n.end - n.onset) * bpm / 60.0, v))
    lines = ['# %s gain_db %.4f' % (name, 20 * np.log10(rmax))]
    for k in range(0, len(ev), 40):
        lines.append('EVENTS %s: %s' % (name, ' '.join(ev[k:k + 40])))
    return '\n'.join(lines) + '\n'


def main(argv=None):
    ap = argparse.ArgumentParser(description='Notes of a monophonic stem from its f0 track.')
    ap.add_argument('--stem', required=True); ap.add_argument('--name', required=True)
    ap.add_argument('--start', type=float, required=True); ap.add_argument('--end', type=float, required=True)
    ap.add_argument('--bpm', type=float, required=True)
    ap.add_argument('--table', required=True); ap.add_argument('--events', required=True)
    ap.add_argument('--keep-cents', action='store_true')
    ap.add_argument('--f0-range', type=float, nargs=2, default=[30.0, 200.0])
    ap.add_argument('--cut-samples', nargs=3, metavar=('N', 'DIR', 'MANIFEST'))
    ap.add_argument('--samples-log'); ap.add_argument('--source-clip', default='unknown')
    ap.add_argument('--rel-samples', action='store_true')
    a = ap.parse_args(argv)
    sr, x = read_wav(a.stem)
    x = x[int(round(a.start * sr)):int(round(a.end * sr))]
    mono = x.mean(axis=1)
    notes, st = analyze(mono, sr, *a.f0_range)
    levels = [note_level(mono, sr, n) for n in notes]
    with open(a.table, 'w') as f:
        f.write('onset_s\tend_s\tlen_s\tf0_hz\tmidi\tglide_cents\trms\treattack\n')
        for n, lv in zip(notes, levels):
            f.write('%.4f\t%.4f\t%.4f\t%.3f\t%.3f\t%.1f\t%.6f\t%d\n' % (n.onset + a.start, n.end + a.start, n.end - n.onset, n.f0, n.midi, n.glide, lv, int(n.reattack)))
    rel = None
    if a.cut_samples:
        if not a.samples_log:
            sys.exit('bass_notes: --cut-samples needs --samples-log')
        rows = cut_samples(x, sr, notes, st, int(a.cut_samples[0]), a.cut_samples[1], a.cut_samples[2], a.name, a.samples_log, a.source_clip, a.start, a.end, os.path.basename(a.stem).replace('.wav', ''))
        if a.rel_samples:
            roots = [(r[8], r[10]) for r in rows]
            def rel(n):
                d = [(abs(rt - n.midi) + (0.01 if rt < n.midi else 0.0), rr) for rt, rr in roots]
                return min(d)[1]
    open(a.events, 'w').write(events_text(notes, levels, a.name, a.start, a.bpm, a.keep_cents, rel))
    g = [abs(n.glide) for n in notes]
    print('STATS notes=%d dropped=%d joined=%d reattacks=%d median_abs_glide_cents=%.1f voiced_frames_covered=%.4f voiced_frames=%d'
          % (len(notes), st['dropped'], st['joined'], sum(n.reattack for n in notes), float(np.median(g)) if g else float('nan'), st['covered'], st['voiced_frames']))
    return notes, st


if __name__ == '__main__':
    main()
