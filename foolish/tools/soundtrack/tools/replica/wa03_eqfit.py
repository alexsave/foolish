"""wa03_eqfit.py: master EQ string for wa03_play.c (MASTER_EQ) from a score_NN.tsv of compare.py.
Takes the third-octave long-term spectrum differences (replica minus original, lts_diff_*Hz rows), keeps the --n largest |diff| bands
above --min-db, and answers peaking filters (type 3, q 2.2) of gain -0.9 * diff, added to any --base string.
usage: wa03_eqfit.py SCORE.tsv [--n 6] [--min-db 3] [--base "2:40:0.7:0"]   prints the MASTER_EQ value.  Plain python3.
"""
import sys, re, argparse
ap = argparse.ArgumentParser(); ap.add_argument('tsv'); ap.add_argument('--n', type=int, default=6); ap.add_argument('--min-db', type=float, default=3.0); ap.add_argument('--base', default='')
a = ap.parse_args()
d = {}
for ln in open(a.tsv):
    m = re.match(r'lts_diff_([\d.]+)Hz\t([-\d.]+)', ln)
    if m: d[float(m.group(1))] = float(m.group(2))
c = sorted((k for k in d if 40 <= k <= 14000 and abs(d[k]) >= a.min_db), key=lambda k: -abs(d[k]))[:a.n]
out = [a.base] if a.base else []
for k in sorted(c): out.append('3:%g:2.2:%.1f' % (k, max(-12, min(12, -0.9 * d[k]))))
print(','.join(out))
