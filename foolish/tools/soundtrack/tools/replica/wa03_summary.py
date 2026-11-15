"""wa03_summary.py: the before / after / target figure for the wa03 recreation.
usage: wa03_summary.py OUT.png BEFORE.wav AFTER.wav BEFORE.tsv AFTER.tsv FLOOR.tsv [--before-label ..] [--after-label ..]
Three spectrograms (target = the original Lyria clip, before, after) and a bar chart of the key measures: before, after, and the floor (the same
tool scoring another passage of the same clip against this one); the target of every measure is its identity value (the clip against itself).
Run with the venv python (librosa, matplotlib).
"""
import sys, csv, numpy as np, librosa, matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
ORIG = '/Users/alex/Dev/lyria-cities/6_warsaw/wa_03_nocturne_cellos.mp3'
out, bw, aw, bt, at, ft = sys.argv[1:7]
bl = sys.argv[sys.argv.index('--before-label') + 1] if '--before-label' in sys.argv else 'BEFORE (iteration 1)'
al = sys.argv[sys.argv.index('--after-label') + 1] if '--after-label' in sys.argv else 'AFTER (best)'


def meas(p):
    d = {}
    for r in csv.reader(open(p), delimiter='\t'):
        try: d[r[0]] = float(r[1])
        except (ValueError, IndexError): pass
    return d


def spec(ax, f, title):
    y = librosa.load(f, sr=22050, mono=True)[0]
    C = np.abs(librosa.cqt(y, sr=22050, hop_length=512, fmin=librosa.note_to_hz('C2'), n_bins=120, bins_per_octave=24))
    ax.imshow(librosa.amplitude_to_db(C, ref=C.max(), top_db=60), origin='lower', aspect='auto', extent=[0, len(y) / 22050, 36, 96], cmap='magma')
    ax.set_title(title, fontsize=12); ax.set_xlabel('time (s)'); ax.set_ylabel('pitch (MIDI note number)')


fig = plt.figure(figsize=(20, 16)); gs = fig.add_gridspec(4, 1, height_ratios=[1, 1, 1, 1.5], hspace=0.6)
spec(fig.add_subplot(gs[0]), ORIG, 'TARGET: the ORIGINAL Lyria clip (cello and piano together)')
spec(fig.add_subplot(gs[1]), bw, bl + ': our engine, first transcription (piano only, no pedal)')
spec(fig.add_subplot(gs[2]), aw, al + ': our engine, cello and piano from calibrated notes')
b, a, f = meas(bt), meas(at), meas(ft)
keys = [('chroma_cos_above_chance', 'chroma\nabove chance'), ('cqt_cos_above_chance', 'CQT\nabove chance'), ('onset_f30', 'onset F\n(30 ms)'), ('loud400_corr', 'level curve\ncorrelation'), ('bright_corr', 'brightness\ncorrelation')]
ax = fig.add_subplot(gs[3]); x = np.arange(len(keys) + 1); w = 0.2
vals = lambda d: [d.get(k, 0) for k, _ in keys]
ob = [d / 100 for d in [0]]
ov = lambda d: d['overall'] / 100 if 'overall' in d else 0
ax.bar(x - 1.5 * w, [ov(b)] + vals(b), w, label='BEFORE', color='tab:gray')
ax.bar(x - 0.5 * w, [ov(a)] + vals(a), w, label='AFTER', color='tab:green')
ax.bar(x + 0.5 * w, [ov(f)] + vals(f), w, label='FLOOR (another passage of the same clip)', color='tab:orange')
ax.bar(x + 1.5 * w, [1] * (len(keys) + 1), w, label='TARGET (identity: the clip against itself)', color='tab:blue')
ax.set_xticks(x); ax.set_xticklabels(['OVERALL\n(score / 100)'] + [l for _, l in keys]); ax.set_ylim(0, 1.08); ax.legend(loc='upper right'); ax.set_ylabel('measure (1 = identical)')
ax.set_title('key measures from compare.py: before, after, floor, target', fontsize=12)
for i, d in enumerate([ov(b), ov(a)]):
    pass
fig.suptitle('WARSAW wa_03 CELLO NOCTURNE - cello + piano - before, after and target', fontsize=22, fontweight='bold', y=0.97)
fig.text(0.06, 0.02, 'overall score: before %.1f, after %.1f, floor %.1f, target 100' % (b['overall'] if 'overall' in b else 0, a['overall'] if 'overall' in a else 0, f['overall'] if 'overall' in f else 0), fontsize=13)
fig.savefig(out, dpi=55)
