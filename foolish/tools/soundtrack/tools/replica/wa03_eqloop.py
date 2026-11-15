"""wa03_eqloop.py: fit the master EQ of the wa03 player by closed loop.
Renders NOTES with wa03_build.sh, reads the third-octave differences (replica minus original) from compare.py, and moves each band's gain by
-0.5 * its remaining difference (the bands overlap, so a full correction overshoots); repeats --rounds times and prints the final MASTER_EQ.
usage: wa03_eqloop.py NOTES.tsv WORKDIR [--rounds 4] [--bands 3150,4000,5000,6300,8000]; env CELLO_EQ PIANO_EQ as for wa03_build.sh.  Plain python3 -I.
"""
import sys, os, subprocess, re, argparse
HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser(); ap.add_argument('notes'); ap.add_argument('work'); ap.add_argument('--rounds', type=int, default=4)
ap.add_argument('--bands', default='3150,4000,5000,6300,8000,12500'); ap.add_argument('--base', default='2:40:0.7:0')
a = ap.parse_args()
ORIG = '/Users/alex/Dev/lyria-cities/6_warsaw/wa_03_nocturne_cellos.mp3'
bands = [float(b) for b in a.bands.split(',')]; g = {b: 0.0 for b in bands}
os.makedirs(a.work, exist_ok=True)
for r in range(a.rounds):
    meq = ','.join([a.base] + ['3:%g:2.2:%.2f' % (b, g[b]) for b in bands])
    env = dict(os.environ, MASTER_EQ=meq)
    subprocess.run([os.path.join(HERE, 'wa03_build.sh'), a.notes, a.work + '/r', 'cello=' + os.environ.get('CELLO', 'lyria')], env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(['python3', '-I', os.path.join(HERE, 'compare.py'), '--orig', ORIG, '--replica', a.work + '/r/full.wav', '--out', a.work + '/s'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    d = {}
    for ln in open(a.work + '/s.tsv'):
        m = re.match(r'lts_diff_([\d.]+)Hz\t([-\d.]+)', ln)
        if m: d[float(m.group(1))] = float(m.group(2))
    mad = [l for l in open(a.work + '/s.tsv') if l.startswith('lts_mad_db')][0].split('\t')[1]
    print('round', r, 'lts_mad_db', mad, 'diffs', {b: round(d[b], 1) for b in bands}, file=sys.stderr)
    for b in bands: g[b] = max(-14, min(10, g[b] - 0.5 * d[b]))
print(','.join([a.base] + ['3:%g:2.2:%.2f' % (b, g[b]) for b in bands]))
