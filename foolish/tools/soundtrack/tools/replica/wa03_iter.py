"""wa03_iter.py: score one iteration of the wa03 recreation and draw its figure.

usage: wa03_iter.py N NOTES.tsv REPLICA.wav OUTDIR [--label TEXT] [--no-open]
Runs compare.py (original = the Lyria clip, a benchmark only) on the replica, writes OUTDIR/score_NN.tsv (all measures), draws
OUTDIR/iteration_NN.png (original against replica: transcribed notes over the original CQT with piano and cello apart, onset
times, level curve in dB, chroma, mel spectrogram, with the overall score and key measures written on it), appends a row to
OUTDIR/RESULTS.tsv, and opens the PNG with `open` only when the overall score beat every earlier iteration.
Needs the venv python (librosa, matplotlib).
"""
import sys, os, subprocess, csv, json
import numpy as np, librosa, matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ORIG = '/Users/alex/Dev/lyria-cities/6_warsaw/wa_03_nocturne_cellos.mp3'
KEYS = ['overall', 'onset_f30', 'onset_dev_mad_ms', 'chroma_cos_above_chance', 'cqt_cos_above_chance', 'lts_mad_db', 'loud400_corr', 'bright_corr',
        'tempo_pct_diff', 'microtiming_profile_mad_ms']


def run_compare(replica, prefix):
    subprocess.run(['nice', 'python3', '-I', os.path.join(HERE, 'compare.py'), '--orig', ORIG, '--replica', replica, '--out', prefix],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    m = {}
    for r in csv.reader(open(prefix + '.tsv'), delimiter='\t'):
        if r[0] == 'metric':
            continue
        try:
            m[r[0]] = float(r[1])
        except (ValueError, IndexError):
            pass
    ov = [l for l in open(prefix + '.md') if l.startswith('Overall score')]
    m['overall'] = float(ov[0].split('**')[1].split('/')[0])
    return m


def pick(m, k):
    for kk in (k, 'mix.' + k):
        if kk in m:
            return m[kk]
    return float('nan')


def load(path, sr=22050):
    return librosa.load(path, sr=sr, mono=True)[0]


def draw(n, notes, rep_wav, m, out, label, best_prev, part='both'):
    yo, yr = load(ORIG), load(rep_wav)
    L = min(len(yo), len(yr)); yo, yr = yo[:L], yr[:L]
    sr, hop = 22050, 512
    fig = plt.figure(figsize=(20, 19))
    gs = fig.add_gridspec(6, 2, height_ratios=[2.2, 2.2, 1.2, 1.2, 1.6, 1.6], hspace=0.75, wspace=0.3)
    tmax = L / sr
    def cqt(y):
        C = np.abs(librosa.cqt(y, sr=sr, hop_length=hop, fmin=librosa.note_to_hz('C2'), n_bins=60 * 2, bins_per_octave=24))
        return librosa.amplitude_to_db(C, ref=np.max(C), top_db=60)
    Co, Cr = cqt(yo), cqt(yr)
    pname = {'both': 'cello + piano', 'piano': 'PIANO part', 'cello': 'CELLO part'}[part]
    shown = [x for x in notes if part == 'both' or x[0] == part]
    for col, (C, name) in enumerate([(Co, 'ORIGINAL (Lyria clip)'), (Cr, 'REPLICA (our engine)')]):
        a = fig.add_subplot(gs[0, col])
        a.imshow(C, origin='lower', aspect='auto', extent=[0, tmax, 36, 96], cmap='magma')
        a.set_title(name + '\n' + pname + ' notes on the spectrogram (white piano, cyan cello)', fontsize=10)
        a.set_xlabel('time (s)')
        for p, s, e, mi, v in shown:
            a.plot([s, e], [mi, mi], '-', color='cyan' if p == 'cello' else 'white', lw=1.6, alpha=.85)
        a.set_ylim(36, 96); a.set_ylabel('pitch (MIDI note number, 60 = C4)')
    # onsets
    a = fig.add_subplot(gs[1, :])
    oo = librosa.onset.onset_detect(y=yo, sr=sr, units='time', backtrack=False)
    orr = librosa.onset.onset_detect(y=yr, sr=sr, units='time', backtrack=False)
    a.vlines(oo, 0.55, 1, color='tab:red', lw=1.2, label='original %d' % len(oo))
    a.vlines(orr, 0, 0.45, color='tab:blue', lw=1.2, label='replica %d' % len(orr))
    for p, s, e, mi, v in notes:
        a.plot([s], [0.5], '|', color='gray', ms=4)
    a.set_yticks([]); a.set_xlim(0, tmax); a.legend(loc='upper right', ncol=2); a.set_xlabel('time (s)'); a.set_title('ONSET TIMES of the whole mix: ORIGINAL (red, top row) against REPLICA (blue, bottom row); grey ticks = transcribed note starts', fontsize=10)
    # level
    a = fig.add_subplot(gs[2, :])
    def lev(y):
        r = librosa.feature.rms(y=y, frame_length=4096, hop_length=hop)[0]
        return 20 * np.log10(r + 1e-6)
    t = np.arange(len(lev(yo))) * hop / sr
    a.plot(t, lev(yo), 'tab:red', label='original'); a.plot(t, lev(yr), 'tab:blue', label='replica'); a.set_ylabel('level (dB, RMS)'); a.set_xlabel('time (s)'); a.legend(loc='lower right'); a.set_xlim(0, tmax)
    a.set_title('LEVEL CURVE of the whole mix: ORIGINAL (red) against REPLICA (blue), 4096-sample RMS window', fontsize=10)
    # chroma
    for col, (y, nm) in enumerate([(yo, 'original'), (yr, 'replica')]):
        a = fig.add_subplot(gs[3, col])
        ch = librosa.feature.chroma_cqt(y=y, sr=sr, hop_length=hop)
        a.imshow(ch, origin='lower', aspect='auto', extent=[0, tmax, 0, 12], cmap='viridis'); a.set_yticks(np.arange(12) + .5); a.set_yticklabels(list('CDEFGAB'[0:0]) or ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'], fontsize=6)
        a.set_title('CHROMA (pitch classes) of the mix: ' + nm.upper(), fontsize=10); a.set_xlabel('time (s)'); a.set_ylabel('pitch class')
    for col, (y, nm) in enumerate([(yo, 'original'), (yr, 'replica')]):
        a = fig.add_subplot(gs[4, col])
        S = librosa.power_to_db(librosa.feature.melspectrogram(y=y, sr=sr, n_mels=96, fmax=10000, hop_length=hop), ref=1e-3, top_db=70)
        a.imshow(S, origin='lower', aspect='auto', extent=[0, tmax, 0, 10], cmap='magma', vmin=-70, vmax=np.max(Co) + 30 if False else S.max())
        a.set_title('SPECTROGRAM of the mix: ' + nm.upper(), fontsize=10); a.set_xlabel('time (s)'); a.set_ylabel('frequency (kHz)')
    a = fig.add_subplot(gs[5, :]); a.axis('off')
    lines = ['iteration %02d  %s' % (n, label), 'OVERALL %.2f / 100  (best before: %s)' % (m['overall'], '%.2f' % best_prev if best_prev is not None else 'none')]
    lines += ['  '.join('%s %.3g' % (k, pick(m, k)) for k in KEYS[1:5]), '  '.join('%s %.3g' % (k, pick(m, k)) for k in KEYS[5:])]
    a.text(0.0, 0.95, '\n'.join(lines), va='top', family='monospace', fontsize=12)
    fig.suptitle('WARSAW wa_03 CELLO NOCTURNE - %s - original vs replica - iteration %d' % (pname, n), fontsize=22, fontweight='bold', y=0.985)
    fig.savefig(out, dpi=55); plt.close(fig)


def main():
    n = int(sys.argv[1]); tsv, wav, od = sys.argv[2:5]
    label = sys.argv[sys.argv.index('--label') + 1] if '--label' in sys.argv else ''
    os.makedirs(od, exist_ok=True)
    notes = []
    for ln in open(tsv):
        if ln.strip() and not ln.startswith('#'):
            p, s, e, mi, v = ln.split('\t')[:5]; notes.append((p, float(s), float(e), float(mi), float(v)))
    pref = os.path.join(od, 'score_%02d' % n)
    m = run_compare(wav, pref)
    rf = os.path.join(od, 'RESULTS.tsv')
    prev = [r for r in csv.DictReader(open(rf), delimiter='\t')] if os.path.exists(rf) else []
    best_prev = max([float(r['overall']) for r in prev], default=None)
    png = os.path.join(od, 'iteration_%02d.png' % n)
    draw(n, notes, wav, m, png, label, best_prev)
    for part in ('piano', 'cello'):
        if any(x[0] == part for x in notes):
            draw(n, notes, wav, m, png.replace('.png', '_%s.png' % part), label, best_prev, part)
    new = not os.path.exists(rf)
    with open(rf, 'a') as f:
        if new:
            f.write('\t'.join(['iter', 'label'] + KEYS) + '\n')
        f.write('\t'.join([str(n), label] + ['%.4g' % pick(m, k) for k in KEYS]) + '\n')
    improved = best_prev is None or m['overall'] > best_prev
    print('iteration %d: overall %.2f%s' % (n, m['overall'], '' if best_prev is None else ' (best before %.2f)' % best_prev))
    if best_prev is not None:
        for k in KEYS[1:]:
            pv = [float(r[k]) for r in prev if r[k] not in ('', 'nan')]
            if pv and not np.isnan(pick(m, k)):
                better = pick(m, k) < min(pv) if k in ('onset_dev_mad_ms', 'lts_mad_db', 'microtiming_profile_mad_ms') else (abs(pick(m, k)) < min(abs(x) for x in pv) if k == 'tempo_pct_diff' else pick(m, k) > max(pv))
                if better:
                    print('  improved: %s -> %.4g' % (k, pick(m, k))); improved = True
    if improved and '--no-open' not in sys.argv:
        for q in (png, png.replace('.png', '_piano.png'), png.replace('.png', '_cello.png')):
            if os.path.exists(q):
                subprocess.run(['open', q]); print('  opened', q)


if __name__ == '__main__':
    main()
