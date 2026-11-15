# analyze_clip.py - measure one audio file and write a Markdown report, a spectrogram PNG and one summary TSV row.
#
# What it does: runs the lib.py measurements (loudness, bands, tempo map, beat grid, key, sections, chords,
#   bass and melody per bar inside the windows you name, a voice heuristic, ending class) and writes them out.
# Inputs: AUDIO (any ffmpeg-decodable file), --out DIR, optional --prompt FILE (a text prompt; BPM and key are
#   read from it by regular expression), optional --window START END VERDICT NOTE (repeatable; START and END are
#   seconds or m:ss, END may be `end`, START may be `LAST` for the last 10 s), --key LABEL, --title TITLE.
# Outputs: DIR/<stem>.md, DIR/<stem>.png, and the summary row (header plus row) in --tsv (default
#   DIR/<stem>.summary.tsv). Without --window the per-bar tables cover the whole file.
# How to run:
#   python3 -I analyze_clip.py song.mp3 --out out_dir --prompt song.txt --window 30 70 keep "good drop" --window 90 end drop ""
# Known failure modes: tempo octave errors and weak-pulse files (the md says so); one global key per file;
#   ffmpeg must be on PATH; the per-bar tables are only as good as the beat grid (see README.md).
import sys, os, re, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from scipy import signal, ndimage
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from lib import *

def ts(t):
    t = round(max(0, t), 1); return '%d:%04.1f' % (int(t//60), t-60*int(t//60))

def parse_prompt(txt):
    bpms = []
    for m in re.finditer(r'(\d{2,3})\s*BPM', txt, flags=re.I):
        v = int(m.group(1))
        if v not in bpms: bpms.append(v)
    keys = []
    for m in re.finditer(r'\b([A-G][#b]?)[ -](major|minor)\b', txt):
        k = m.group(1)+' '+m.group(2)
        if k not in keys: keys.append(k)
    return bpms, keys

def key_pc(k):
    r, mode = k.split(' ')
    names = {'C':0,'C#':1,'Db':1,'D':2,'D#':3,'Eb':3,'E':4,'F':5,'F#':6,'Gb':6,'G':7,'G#':8,'Ab':8,'A':9,'A#':10,'Bb':10,'B':11}
    return names[r], mode == 'minor'

def key_vs_prompt(meas, pkeys):
    if not pkeys: return ''
    mp = key_pc(meas)
    out = []
    for k in pkeys:
        pp = key_pc(k)
        if pp == mp: out.append('match'); continue
        if pp[1] != mp[1]:
            minor = pp if pp[1] else mp; major = mp if pp[1] else pp
            if (minor[0]+3) % 12 == major[0]: out.append('relative'); continue
            if minor[0] == major[0]: out.append('parallel'); continue
        d = (mp[0]-pp[0]) % 12
        if d in (5, 7) and mp[1] == pp[1]: out.append('fifth-related'); continue
        out.append('differs')
    best = 'match' if 'match' in out else ('relative' if 'relative' in out else out[0])
    return best

def tempo_vs_prompt(best, second, pb):
    if not pb: return ''
    parts = []
    for p in pb:
        rb = best/p
        s = 'x%.2f of %d' % (rb, p)
        parts.append(s)
    return '; '.join(parts)

def ratio_label(r):
    for v, nm in ((2, 'x2'), (0.5, 'x0.5'), (1.5, 'x1.5'), (2/3., 'x2/3'), (3, 'x3'), (1/3., 'x1/3'), (4/3., 'x4/3'), (0.75, 'x3/4')):
        if abs(r/v-1) < 0.06: return nm
    return None

def plot(r, secs, wins, out_png, title, loud):
    cen, fq, db = spec_image(r['mono44'])
    top = db.max()
    fig, ax = plt.subplots(2, 1, figsize=(12, 6), sharex=True, gridspec_kw=dict(height_ratios=[3, 1], hspace=0.06))
    dur = r['dur']
    ax[0].imshow(db.T, origin='lower', aspect='auto', extent=[cen[0], cen[-1], 0, len(fq)], vmin=top-90, vmax=top, cmap='magma', interpolation='nearest')
    ticks = [30, 100, 300, 1000, 3000, 10000]
    ax[0].set_yticks([np.argmin(abs(fq-f)) for f in ticks]); ax[0].set_yticklabels([str(f) for f in ticks], fontsize=7)
    for s in secs[1:-1]: 
        ax[0].axvline(s, color='w', ls='--', lw=0.7)
        ax[1].axvline(s, color='k', ls='--', lw=0.5)
    for (a, b, lab) in wins:
        ax[0].axvspan(a, b, ymin=0.95, ymax=1.0, color='cyan', alpha=0.8)
        ax[1].axvspan(a, b, color='cyan', alpha=0.25)
    ax[0].set_ylabel('Hz', fontsize=8); ax[0].set_title(title+'  (dashed: section boundaries, cyan: windows)', fontsize=9)
    t, l = loud.curve()
    ax[1].plot(t, l, 'k', lw=0.8); ax[1].set_ylabel('short-term LUFS (3 s)', fontsize=7)
    ax[1].set_ylim(max(-60, np.nanmin(l[l > -100])-3 if (l > -100).any() else -60), 0)
    ax[1].set_xlim(0, dur); ax[1].tick_params(labelsize=7); ax[1].set_xlabel('s', fontsize=8)
    fig.subplots_adjust(left=0.06, right=0.99, top=0.94, bottom=0.08)
    for dpi in (80, 65, 50):
        fig.savefig(out_png, dpi=dpi)
        if os.path.getsize(out_png) < 400000: break
    plt.close(fig)

# ------------------------------------------------------------ md
def write_md(path_md, name, title, r, row, secs, loud, wins, prompt, tables, voice, notes, windows_label_rows):
    L = []
    L.append('# %s' % title)
    L.append('')
    L.append('All values are DSP measurements (see README.md in the analysis tools folder for the method and failure modes).')
    L.append('')
    if prompt: L.append('Prompt: `%s`' % prompt.strip().replace('\n', ' ')[:600]); L.append('')
    if windows_label_rows:
        L.append('## Windows of interest'); L.append('')
        L.append('| start | end | verdict | note |'); L.append('|---|---|---|---|')
        for a, b, v, n in windows_label_rows: L.append('| %s | %s | %s | %s |' % (ts(a), ts(b), v, n))
        L.append('')
    L.append('![spectrogram](%s)' % (os.path.basename(path_md)[:-3]+'.png')); L.append('')
    L.append('## Summary numbers'); L.append('')
    for k, v in row.items(): L.append('- %s: %s' % (k, v))
    L.append('')
    for n in notes: L.append('- NOTE: %s' % n)
    L.append('')
    # tempo map
    L.append('## Tempo map (8 s windows, hop 4 s, onset-envelope autocorrelation)'); L.append('')
    L.append('Tempo is the Viterbi-smoothed pick over the top 5 autocorrelation peaks of every window (penalty 0.5 per octave of change between neighbours); raw is the single strongest peak of that window alone.'); L.append('')
    L.append('| window | tempo BPM (smoothed) | raw strongest peak BPM | pulse strength | flag |'); L.append('|---|---|---|---|---|')
    prevb = None; steps = []
    for (t0, t1, b, st, pk, raw) in r['wins']:
        flag = 'weak pulse' if st < 0.15 else ''
        L.append('| %s-%s | %s | %s | %.2f | %s |' % (ts(t0), ts(t1), '%.1f' % b if not np.isnan(b) else 'n/a', '%.1f' % raw if not np.isnan(raw) else 'n/a', st, flag))
        if prevb is not None and not np.isnan(b) and not np.isnan(prevb[0]) and prevb[1] >= 0.15 and st >= 0.15:
            q = b/prevb[0]
            if abs(q-1) > 0.03:
                lab = ratio_label(q)
                steps.append('%s: %.1f -> %.1f BPM (%+.1f%%)%s' % (ts(t0+4), prevb[0], b, 100*(q-1), (' - ratio looks like an octave/meter flip of the estimator (%s), not necessarily a real change' % lab) if lab else ''))
        prevb = (b, st)
    L.append('')
    L.append('Steps larger than 3%% between neighbouring windows (time = start of the later window plus 4 s; windows with pulse strength below 0.15 skipped): %d' % len(steps)); L.append('')
    for s in steps: L.append('- %s' % s)
    if not steps: L.append('- none')
    L.append('')
    # beats
    L.append('## Beat grid'); L.append('')
    beats = r['beats']
    if len(beats) < 8:
        L.append('Not enough beats tracked.'); L.append('')
    else:
        bars = r['bars']
        bl = np.diff(bars)
        L.append('Beats per bar assumed: %d (meter evidence z=%.2f, lead over the alternative %.2f; weak if below about 1.5).' % (r['m'], r['mz'], r['mmarg'] if not np.isnan(r['mmarg']) else float('nan')))
        L.append('Bar length: median %.2f s, 10th-90th percentile %.2f-%.2f s (%d bars). Beat period median %.3f s.' % (np.median(bl), np.percentile(bl, 10), np.percentile(bl, 90), len(bl), np.median(np.diff(beats))))
        L.append('')
        L.append('Time (s) of every 4th tracked beat (starting at beat 1):'); L.append('')
        L.append(', '.join('%.2f' % t for t in beats[::4])); L.append('')
        L.append('Bar start times (downbeat phase chosen by low-band/onset accent, weak evidence): first strong downbeat-like beat at %.2f s.' % r['first_down']); L.append('')
    # sections
    L.append('## Sections (checkerboard novelty on chroma + band energies)'); L.append('')
    L.append('| # | start | end | dur s | LUFS-like | tempo BPM | sub dB | bass dB | lowmid dB | mid dB | high dB | air dB | onsets/s | percussive share |'); L.append('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    for i in range(len(secs)-1):
        s = stats(r, loud, secs[i], secs[i+1])
        sh = s['shares']
        L.append('| %d | %s | %s | %.1f | %.1f | %.1f | %s | %.2f | %.2f |' % (i+1, ts(secs[i]), ts(secs[i+1]), secs[i+1]-secs[i], s['lufs'], s['tempo'], ' | '.join('%.1f' % v for v in sh), s['onset'], s['perc']))
    L.append('')
    L.append('Band shares are dB relative to the total energy of the section.')
    L.append('Boundaries (s): %s' % ', '.join('%.1f' % t for t in secs[1:-1])); L.append('')
    # tables
    for head, rows in tables:
        L.append('## ' + head); L.append('')
        L += rows; L.append('')
    if voice is not None:
        L.append('## Possible voice (unverified heuristic)'); L.append('')
        L.append('This is an unverified heuristic.')
        L.append('It scores each 2 s block from the energy share in 300-3400 Hz, the harmonic share of that band, 4-8 Hz pitch modulation of the strongest 150-1000 Hz pitch, and broad spectral peaks.')
        L.append('It cannot tell a choir from strings, a flute, a lead synth or any other sustained vibrato instrument.')
        L.append('')
        rng = merge_ranges(voice)
        if rng:
            L.append('Time ranges where the 2 s score is at least %.2f (the file 90th percentile, clipped to 0.35-0.50; peak score and its energy/harmonic/vibrato/formant terms in brackets):' % voice_threshold(voice)); L.append('')
            for a, b, s, tm in rng: L.append('- %s-%s (%.2f; %.2f/%.2f/%.2f/%.2f)' % (ts(a), ts(b), s, *tm))
        else:
            L.append('No 2 s block reached the threshold.')
        L.append('')
        L.append('Per-block scores (start second:score), score = 0.2 energy + 0.3 harmonic + 0.3 vibrato + 0.2 formant-peak term, each term 0-1:'); L.append('')
        L.append(' '.join('%d:%.2f' % (int(round(a)), sc) for a, b, sc, *_ in voice)); L.append('')
    with open(path_md, 'w') as f: f.write('\n'.join(L)+'\n')

# ------------------------------------------------------------ summary columns
# internal row name -> column name in the summary TSV (names not listed keep their row name)
TSV_NAMES = {'LUFS': 'LUFS_integrated', 'LRA': 'loudness_range_LU', 'BPM': 'measured_BPM', 'BPM_second': 'second_BPM_candidate', 'tempo_std_BPM': 'tempo_stability_std_BPM',
         'key': 'measured_key', 'key_conf': 'key_confidence_corr_gap', 'tuning_cents': 'tuning_offset_cents', 'sub_dB': 'sub_lt60_dB', 'bass_dB': 'bass_60_250_dB', 'lowmid_dB': 'lowmid_250_1000_dB',
         'mid_dB': 'mid_1000_4000_dB', 'high_dB': 'high_4000_10000_dB', 'air_dB': 'air_gt10000_dB', 'side_mid_dB': 'side_to_mid_dB', 'onsets_per_s': 'onset_density_per_s',
         'percussive_share': 'percussive_share_HPSS', 'last_second_dB': 'last_second_level_dBFS', 'ending': 'ending_type', 'first_downbeat_s': 'first_downbeat_like_beat_s',
         'prompt_BPM': 'prompt_BPM', 'prompt_key': 'prompt_key'}

def f1(v, d=1): return ('%.'+str(d)+'f') % v if v == v else ''

def mmss(s):
    m, x = s.split(':'); return int(m)*60+float(x)

def parse_time(tok, dur, is_end):
    """seconds, m:ss, `end` (the file end) or `LAST` (10 s before the end)."""
    t = tok.strip()
    if t.lower() == 'end': return dur
    if t == 'LAST': return max(0., dur-10)
    return mmss(t) if ':' in t else float(t)

def tsv_text(row):
    names = [TSV_NAMES.get(h, h) for h in row.keys()]
    return '\t'.join(names)+'\n'+'\t'.join(str(v) for v in row.values())+'\n'

def analyze_file(path, key, title, prompt_text, windows, md_path, want_md=True):
    """Measure one file.  windows: list of (start_token, end_token, verdict, note) (see parse_time).
    Returns the summary row (dict, internal names; the first item is `file`).  Writes md and png when want_md."""
    r = run_core(path)
    dur = r['dur']
    ptxt = prompt_text or ''
    pb, pk = parse_prompt(ptxt)
    loud = Loud(r['mono44'], 44100)
    notes = []
    if r['pulse'] < 0.2: notes.append('weak pulse: strongest autocorrelation peak is %.2f (below 0.20), so tempo, beat grid, bars, chords per bar are low confidence' % r['pulse'])
    if r['tstd'] > 0.03*r['bpm'] if r['bpm'] == r['bpm'] else False: notes.append('tempo is not stable (std of 8 s windows folded to the global octave is %.1f BPM)' % r['tstd'])
    if r['kconf'] < 0.05: notes.append('key evidence is weak: runner-up %s is within %.3f correlation of %s' % (r['key2'], r['kconf'], r['key']))
    if r['tuneR'] < 0.3: notes.append('tuning estimate is unreliable (peak-deviation concentration %.2f), cents offset may be noise' % r['tuneR'])
    # tempo relation to prompt
    tnote = []
    for p in pb:
        for lab, b in (('best', r['bpm']), ('second', r['bpm2'])):
            if b != b: continue
            q = b/p
            rl = ratio_label(q) if abs(q-1) > 0.03 else 'same'
            if rl == 'same': tnote.append('%s measured %.1f BPM agrees with prompt %d BPM (within 3%%)' % (lab, b, p))
            elif rl:
                s = '%s measured %.1f BPM is %s of prompt %d BPM' % (lab, b, rl, p)
                if rl in ('x2', 'x0.5'):
                    slow = min(b, p)
                    if 2*slow <= 230:
                        ratio = level_support(r['env'], r['fs'], slow)
                        s += '; odd/even onset-strength ratio at the %.1f BPM grid is %.2f (above about 0.7 supports the faster level %.1f BPM, below 0.5 supports the slower level; between is undecided)' % (slow, ratio, 2*slow)
                else:
                    s += '; this ratio is not resolved by the odd/even test'
                tnote.append(s)
            else:
                tnote.append('%s measured %.1f BPM is %.2fx prompt %d BPM (not a simple multiple)' % (lab, b, q, p))
    if len(pb) > 1: tnote.append('prompt lists several tempos (%s), so a single global BPM can only match one of them' % ', '.join(map(str, pb)))
    if r['doubled']: notes.append('estimator doubled its first pick (%.1f BPM) because the odd/even onset-strength ratio was %.2f (>= 0.85)' % (r['bpm2'], r['octr']))
    elif r['octr'] == r['octr'] and r['octr'] >= 0.6: notes.append('octave doubt: onset strength halfway between tracked beats is %.2f of that on the beats, so %.1f BPM is also plausible' % (r['octr'], 2*r['bpm']))
    last1, kind, drop, tail = r['end']
    bs = r['bandE'].sum(0); sh = 10*np.log10(bs/bs.sum()+1e-12)
    kv = key_vs_prompt(r['key'], pk)
    row = dict(file=key, duration_s=f1(dur, 2), LUFS=f1(r['lufs']), true_peak_dBTP=f1(r['tp']), LRA=f1(r['lra']),
               prompt_BPM='/'.join(map(str, pb)), BPM=f1(r['bpm']), BPM_second=f1(r['bpm2']), tempo_std_BPM=f1(r['tstd'], 2),
               first_downbeat_s=f1(r['first_down'], 2), prompt_key='/'.join(pk), key=r['key'], key_conf=f1(r['kconf'], 3), tuning_cents=f1(r['tuning']),
               centroid_Hz=f1(r['centroid'], 0), sub_dB=f1(sh[0]), bass_dB=f1(sh[1]), lowmid_dB=f1(sh[2]), mid_dB=f1(sh[3]), high_dB=f1(sh[4]), air_dB=f1(sh[5]),
               side_mid_dB=f1(r['sm_db']), onsets_per_s=f1(r['onset_dens'], 2), percussive_share=f1(r['perc_share'], 2), last_second_dB=f1(last1), ending=kind,
               tempo_vs_prompt='; '.join('%.2fx of %d' % (r['bpm']/p, p) for p in pb), key_vs_prompt=kv, pulse_strength=f1(r['pulse'], 2),
               octave_odd_even=f1(r['octr'], 2), key_runner_up=r['key2'], beats_per_bar=r['m'], meter_z=f1(r['mz'], 2),
               end_drop_dB=f1(drop), end_tail_s=f1(tail))
    if want_md:
        os.makedirs(os.path.dirname(os.path.abspath(md_path)), exist_ok=True)
        secs = sections(r)
        wl = [(parse_time(a, dur, False), parse_time(b, dur, True), v, n) for a, b, v, n in windows]
        row_md = {k: v for k, v in row.items() if k != 'file'}
        row_md['tempo_vs_prompt_detail'] = ' | '.join(tnote) if tnote else ('no prompt tempo' if not pb else '')
        row_md['key_vs_prompt'] = '%s (prompt %s)' % (kv, ', '.join(pk)) if pk else 'no prompt key'
        row_md['ending_detail'] = '%s; level of last 100 ms is %.1f dB below the 75th percentile level of the preceding seconds; the level stays within 6 dB of that reference until %.1f s before the end' % (kind, drop, tail)
        png = md_path[:-3]+'.png'
        plot(r, secs, [(a, b, v) for a, b, v, n in wl], png, title, loud)
        tables = []
        tw = wl if wl else [(0., dur, 'whole file', '')]
        if len(r['beats']) >= 8:
            for a, b, v, n in tw:
                head = 'Per-bar table %s-%s (%s)' % (ts(a), ts(b), v)
                try:
                    tb = bar_table(r, a, b, r['tuning'])
                    if r['pulse'] < 0.2 or r['tstd'] > 0.03*r['bpm']:
                        tb = ['LOW CONFIDENCE: the beat tracker had weak or unstable pulse evidence in this file, so bar boundaries are approximate and chords, bass and melody per bar are shifted or smeared accordingly.', ''] + tb
                    tables.append((head, tb))
                except Exception as ex:
                    tables.append((head, ['bar table failed: %r' % ex]))
        else:
            tables.append(('Per-bar table', ['Not produced: too few tracked beats.']))
        voice = voice_heuristic(r, r['tuning'])
        write_md(md_path, key, title, r, row_md, secs, loud, wl, ptxt, tables, voice, notes, wl)
    return row

def read_text(p):
    with open(p) as f: return f.read()

def main():
    ap = argparse.ArgumentParser(description='Measure one audio file: Markdown report, spectrogram PNG, one summary TSV row.')
    ap.add_argument('audio')
    ap.add_argument('--out', required=True, help='output directory (created)')
    ap.add_argument('--prompt', help='text file with the generation prompt (BPM and key are read from it)')
    ap.add_argument('--window', nargs=4, action='append', default=[], metavar=('START', 'END', 'VERDICT', 'NOTE'),
                    help='window of interest: seconds or m:ss; END may be end; START may be LAST (last 10 s); repeatable')
    ap.add_argument('--key', help='label for the file column and the md heading source (default: the file name)')
    ap.add_argument('--title', help='md heading (default: the --key label without extension)')
    ap.add_argument('--tsv', help='summary TSV path (default OUT/<stem>.summary.tsv)')
    a = ap.parse_args()
    stem = os.path.splitext(os.path.basename(a.audio))[0]
    key = a.key or os.path.basename(a.audio)
    title = a.title or os.path.splitext(key)[0]
    os.makedirs(a.out, exist_ok=True)
    md = os.path.join(a.out, stem+'.md')
    row = analyze_file(a.audio, key, title, read_text(a.prompt) if a.prompt else '', a.window, md)
    tsv = a.tsv or os.path.join(a.out, stem+'.summary.tsv')
    with open(tsv, 'w') as f: f.write(tsv_text(row))
    print('wrote', md, md[:-3]+'.png', tsv)

if __name__ == '__main__':
    main()
