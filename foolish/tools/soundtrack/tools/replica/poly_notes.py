# poly_notes.py - EVENTS for a polyphonic pitched part from transcribed notes (basic-pitch rows) and the stem they came from.
#
# What it does: takes the rows of --notes (tsv: stem, onset_s, duration_s, midi, name, velocity) whose stem is in --stems
#   (default other, vocals) and whose onset lies in [start, end); drops rows shorter than 50 ms and rows whose transcription
#   velocity is under --min-vel (default none; the distribution of the transcription velocities is printed); moves each onset to
#   the nearest onset compare.py's detector finds in the stem when one lies within 50 ms (else keeps the transcribed time; both
#   cases counted; no grid is ever used); groups the notes that start within 20 ms of the group's first onset; the velocity
#   of a group is the stem's 30 ms RMS starting at the group's first onset, shared by its notes, mapped as extract.py does it
#   (vel = clip(r / r_max - 0.05, 0, 0.95), part gain 20 log10 r_max on the "# PART gain_db X" line); a note's length is as
#   transcribed but ends 10 ms before the next onset of the same pitch (a second row of the same pitch within 12 ms of
#   another is a duplicate and is dropped, counted). Split by role: HOOK = at each group the highest note when it is at or above
#   --hook-min (default MIDI 67), CHORD = every other note.
# Inputs: --notes TSV --stem WAV --start S --end S --bpm B --out-prefix P [--min-vel V] [--hook-min M] [--stems other vocals]
# Outputs: P_hook.txt and P_chord.txt (EVENTS files, "EVENTS HOOK: beat,pitch,len,vel ..." lines of at most 40 events, beat =
#   (onset - start) * bpm / 60 with 4 decimals, pitch the integer MIDI number), P_table.tsv (every kept note and its history),
#   and a STATS line on stdout.
# How to run: python3 -I poly_notes.py --notes n.tsv --stem other.wav --start 0 --end 42 --bpm 160 --out-prefix out/poly
# Known failure modes: the transcription's ghost notes stay unless --min-vel drops them; a stem onset that belongs to another
#   instrument can attract a snap; the 30 ms RMS of a group includes everything in the stem at that moment, not only the group.
# Randomness: none.
import argparse, csv, os, sys
import numpy as np
from scipy.io import wavfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare as C

SNAP_S = 0.050
SNAP_MODE = 'detector'         # 'detector' | 'grid16' | 'none' (the last two exist so the test can mutate this rule)
MIN_LEN_S = 0.050
GROUP_S = 0.020
VEL_WIN_S = 0.030
SAME_PITCH_GAP_S = 0.010
OVERLAP_RULE = True            # end a note 10 ms before the next onset of the same pitch
DUP_S = 0.012
SPLIT_INVERTED = False         # the test mutates this: HOOK takes the lowest note instead of the highest
BPM_FOR_GRID = [160.0]


def read_wav(path):
    sr, x = wavfile.read(path)
    if x.dtype == np.int16:
        x = x / 32768.0
    x = x.astype(np.float64)
    return sr, (x.mean(axis=1) if x.ndim == 2 else x)


def read_rows(path, stems, start, end):
    rows = []
    with open(path) as f:
        rd = csv.reader(f, delimiter='\t')
        hdr = next(rd)
        assert hdr[:6] == ['stem', 'onset_s', 'duration_s', 'midi', 'name', 'velocity'], hdr
        for r in rd:
            if r[0] in stems and start <= float(r[1]) < end:
                rows.append(dict(stem=r[0], onset=float(r[1]) - start, dur=float(r[2]), midi=int(round(float(r[3]))), tvel=float(r[5])))
    return rows


def snap(t, det):
    if SNAP_MODE == 'none':
        return t, False
    if SNAP_MODE == 'grid16':
        g = 60.0 / BPM_FOR_GRID[0] / 4
        return round(t / g) * g, True
    if len(det) == 0:
        return t, False
    k = int(np.argmin(np.abs(det - t)))
    if abs(det[k] - t) <= SNAP_S:
        return float(det[k]), True
    return t, False


def process(rows, mono, sr, start, bpm, min_vel=None, hook_min=67):
    """rows: dicts as read_rows makes them (times relative to the window). Returns (notes, info)."""
    BPM_FOR_GRID[0] = bpm
    info = dict(rows=len(rows))
    rows = [r for r in rows if r['dur'] >= MIN_LEN_S]
    info['after_min_len'] = len(rows)
    tv = np.array([r['tvel'] for r in rows])
    info['tvel_quantiles'] = {q: float(np.percentile(tv, q)) for q in (0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100)} if len(tv) else {}
    if min_vel is not None:
        rows = [r for r in rows if r['tvel'] >= min_vel]
    info['after_min_vel'] = len(rows)
    det, _, _ = C.detect_onsets(C.resample(mono, sr))
    n_snap = 0
    for r in rows:
        r['t_tr'] = r['onset']
        r['onset'], r['snapped'] = snap(r['onset'], det)
        n_snap += r['snapped']
    info['snapped'] = n_snap; info['kept_transcribed_time'] = len(rows) - n_snap
    # same pitch: duplicates, then the overlap rule
    nd = 0
    keep = []
    for p in sorted(set(r['midi'] for r in rows)):
        rp = sorted([r for r in rows if r['midi'] == p], key=lambda r: (r['onset'], -r['dur']))
        out = []
        for r in rp:
            if out and r['onset'] - out[-1]['onset'] < DUP_S:
                nd += 1
                if r['dur'] > out[-1]['dur']:
                    out[-1] = r
                continue
            out.append(r)
        for a, b in zip(out, out[1:]):
            if OVERLAP_RULE and a['onset'] + a['dur'] > b['onset'] - SAME_PITCH_GAP_S:
                a['dur'] = max(0.005, b['onset'] - SAME_PITCH_GAP_S - a['onset'])
        keep += out
    info['duplicates'] = nd
    rows = sorted(keep, key=lambda r: (r['onset'], r['midi']))
    # groups and velocities
    gi = -1; first = -1e9
    for r in rows:
        if r['onset'] - first > GROUP_S:
            gi += 1; first = r['onset']
        r['group'] = gi
    ng = gi + 1
    rv = np.zeros(ng)
    for g in range(ng):
        t0 = min(r['onset'] for r in rows if r['group'] == g)
        a = int(round(t0 * sr)); seg = mono[a:a + int(VEL_WIN_S * sr)]
        rv[g] = float(np.sqrt(np.mean(seg ** 2))) if len(seg) else 0.0
    rmax = float(rv.max()) if ng else 1.0
    for r in rows:
        r['r'] = float(rv[r['group']]); r['vel'] = float(np.clip(rv[r['group']] / rmax - 0.05, 0, 0.95))
    # split by role
    for g in range(ng):
        mem = [r for r in rows if r['group'] == g]
        top = min(mem, key=lambda r: r['midi']) if SPLIT_INVERTED else max(mem, key=lambda r: r['midi'])
        for r in mem:
            r['part'] = 'HOOK' if (r is top and r['midi'] >= hook_min) else 'CHORD'
    info['groups'] = ng; info['gain_db'] = 20 * np.log10(max(rmax, 1e-12))
    info['hook'] = sum(r['part'] == 'HOOK' for r in rows); info['chord'] = sum(r['part'] == 'CHORD' for r in rows)
    return rows, info


def events_text(rows, part, bpm, gain_db):
    ev = ['%.4f,%d,%.4f,%.4f' % (r['onset'] * bpm / 60.0, r['midi'], r['dur'] * bpm / 60.0, r['vel']) for r in rows if r['part'] == part]
    lines = ['# %s gain_db %.4f' % (part, gain_db)]
    for k in range(0, len(ev), 40):
        lines.append('EVENTS %s: %s' % (part, ' '.join(ev[k:k + 40])))
    return '\n'.join(lines) + '\n'


def main(argv=None):
    ap = argparse.ArgumentParser(description='EVENTS for a polyphonic pitched part from transcribed notes and the stem.')
    ap.add_argument('--notes', required=True); ap.add_argument('--stem', required=True)
    ap.add_argument('--start', type=float, required=True); ap.add_argument('--end', type=float, required=True)
    ap.add_argument('--bpm', type=float, required=True); ap.add_argument('--out-prefix', required=True)
    ap.add_argument('--min-vel', type=float, default=None); ap.add_argument('--hook-min', type=int, default=67)
    ap.add_argument('--stems', nargs='+', default=['other', 'vocals'])
    a = ap.parse_args(argv)
    sr, x = read_wav(a.stem)
    mono = x[int(round(a.start * sr)):int(round(a.end * sr))]
    rows = read_rows(a.notes, a.stems, a.start, a.end)
    rows, info = process(rows, mono, sr, a.start, a.bpm, a.min_vel, a.hook_min)
    d = os.path.dirname(a.out_prefix)
    if d:
        os.makedirs(d, exist_ok=True)
    for part in ('HOOK', 'CHORD'):
        open('%s_%s.txt' % (a.out_prefix, part.lower()), 'w').write(events_text(rows, part, a.bpm, info['gain_db']))
    with open(a.out_prefix + '_table.tsv', 'w') as f:
        f.write('part\tstem\tonset_s\tonset_transcribed_s\tsnapped\tlen_s\tmidi\ttrans_vel\tgroup\tr\tvel\n')
        for r in rows:
            f.write('%s\t%s\t%.4f\t%.4f\t%d\t%.4f\t%d\t%.0f\t%d\t%.6f\t%.4f\n' % (r['part'], r['stem'], r['onset'] + a.start, r['t_tr'] + a.start, r['snapped'], r['dur'], r['midi'], r['tvel'], r['group'], r['r'], r['vel']))
    print('transcription velocity quantiles (after the 50 ms length drop): ' + ' '.join('%d%%=%.0f' % (q, v) for q, v in info['tvel_quantiles'].items()))
    print('STATS rows=%d after_min_len=%d after_min_vel=%d snapped=%d kept_transcribed_time=%d duplicates=%d groups=%d hook=%d chord=%d gain_db=%.3f'
          % (info['rows'], info['after_min_len'], info['after_min_vel'], info['snapped'], info['kept_transcribed_time'], info['duplicates'], info['groups'], info['hook'], info['chord'], info['gain_db']))
    return rows, info


if __name__ == '__main__':
    main()
