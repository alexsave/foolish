"""wa03_results.py: RESULTS.md, every iteration against every measure.
usage: wa03_results.py ITER_DIR OUT.md [--extra "NN=label"] ...  Reads ITER_DIR/RESULTS.tsv (labels) and ITER_DIR/score_NN.{tsv,md}.  Plain python3 -I.
"""
import sys, csv, re, glob, os
d, out = sys.argv[1], sys.argv[2]
labels = {}
for r in csv.DictReader(open(os.path.join(d, 'RESULTS.tsv')), delimiter='\t'):
    if r['iter'] not in ('98', '99'): labels[int(r['iter'])] = r['label']
for i, a in enumerate(sys.argv):
    if a == '--extra': k, v = sys.argv[i + 1].split('=', 1); labels[int(k)] = v
M = ['overall', 'onset_n_replica', 'onset_precision', 'onset_recall', 'onset_f15', 'onset_f30', 'onset_f50', 'onset_dev_mean_ms', 'onset_dev_std_ms', 'tempo_bpm_replica',
     'tempo_pct_diff', 'swing_ms_replica', 'microtiming_profile_mad_ms', 'offset_ms', 'drift_ms', 'kick_f30', 'clap_f30', 'hat_f30', 'chroma_cos_above_chance', 'cqt_cos_above_chance',
     'lts_mad_db', 'loud400_corr', 'loud400_mad_db', 'loud20_corr', 'bright_corr', 'bright_median_ratio']
def meas(i):
    p = os.path.join(d, 'score_%02d' % i); m = {}
    for r in csv.reader(open(p + '.tsv'), delimiter='\t'):
        try: m[r[0]] = float(r[1])
        except (ValueError, IndexError): pass
    m['overall'] = float(re.search(r'Overall score: \*\*([\d.]+)', open(p + '.md').read()).group(1))
    return m
its = sorted(labels)
rows = {i: meas(i) for i in its if os.path.exists(os.path.join(d, 'score_%02d.tsv' % i))}
L = ['# wa03 recreation: every version against every measure', '',
     'Original: wa_03_nocturne_cellos (Lyria, a benchmark only), 30.77 s; tempo_bpm_orig 178.4 by the tool (unreliable on this rubato clip, see the report); onset count of the original 141.',
     'Version columns are iterations; labels below.  All scores by compare.py, mix only (no usable original stems: Demucs leaves piano and cello in one stem).', '',
     '| measure | ' + ' | '.join(str(i) for i in rows) + ' |', '|---|' + '---|' * len(rows)]
for k in M:
    L.append('| %s | ' % k + ' | '.join(('%.4g' % rows[i][k]) if k in rows[i] else 'n/a' for i in rows) + ' |')
L += ['', '## Versions', ''] + ['%d. %s' % (i, labels[i]) for i in rows]
open(out, 'w').write('\n'.join(L) + '\n')
