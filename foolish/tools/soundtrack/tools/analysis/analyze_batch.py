# analyze_batch.py - run analyze_clip on a list of audio files; one report folder plus one summary.tsv.
#
# What it does: for every audio path in the list file, measures it with analyze_clip.analyze_file and appends its
#   row to summary.tsv. Files that have windows in the windows TSV also get a Markdown report and a PNG.
# Inputs: LIST (text file, one audio path per line; blank lines and lines starting with # are skipped),
#   --out DIR, optional --windows TSV (tab separated, one window per line: KEY START END VERDICT NOTE; KEY is
#   `<parent folder>/<file name>`, the full path, or the bare file name; START and END are seconds or m:ss, END may
#   be `end`, START may be `LAST`; lines starting with # are skipped; VERDICT and NOTE may be missing).
#   The prompt of a file is read from the first that exists of: <dir>/<stem>.txt, <dir>/prompts/<stem>.txt,
#   <dir>/../prompts/<parent>/<stem>.txt (BPM and key are read from it by regular expression).
# Outputs: DIR/summary.tsv (header plus one row per file, rewritten after every file), and for files with windows
#   DIR/<parent folder>/<stem>.md and .png.
# How to run:
#   python3 -I analyze_batch.py files.txt --out reports --windows windows.tsv
# Known failure modes: an undecodable file is reported on stderr and skipped (exit code 1 at the end);
#   one file at a time (the work is CPU bound; run one batch at a time on a busy machine); see README.md for the
#   measurement limits.
import sys, os, argparse, traceback
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze_clip import analyze_file, tsv_text, read_text

def read_list(p):
    out = []
    for ln in read_text(p).splitlines():
        ln = ln.strip()
        if ln and not ln.startswith('#'): out.append(ln)
    return out

def read_windows(p):
    W = {}
    for ln in read_text(p).splitlines():
        if ln.startswith('#') or not ln.strip(): continue
        f = ln.split('\t')
        if len(f) < 3: raise SystemExit('windows line needs KEY START END: %r' % ln)
        W.setdefault(f[0], []).append((f[1], f[2], f[3] if len(f) > 3 else '', f[4] if len(f) > 4 else ''))
    return W

def find_prompt(path):
    d = os.path.dirname(os.path.abspath(path)); stem = os.path.splitext(os.path.basename(path))[0]
    parent = os.path.basename(d)
    for c in (os.path.join(d, stem+'.txt'), os.path.join(d, 'prompts', stem+'.txt'), os.path.join(d, '..', 'prompts', parent, stem+'.txt')):
        if os.path.exists(c): return c
    return None

def main():
    ap = argparse.ArgumentParser(description='Run analyze_clip over a list of audio files.')
    ap.add_argument('list', help='text file, one audio path per line')
    ap.add_argument('--out', required=True, help='output directory (created)')
    ap.add_argument('--windows', help='windows TSV (KEY START END VERDICT NOTE)')
    a = ap.parse_args()
    W = read_windows(a.windows) if a.windows else {}
    os.makedirs(a.out, exist_ok=True)
    rows = []; failed = 0
    for path in read_list(a.list):
        parent = os.path.basename(os.path.dirname(os.path.abspath(path)))
        base = os.path.basename(path); stem = os.path.splitext(base)[0]
        key = '%s/%s' % (parent, base)
        wins = W.get(key) or W.get(path) or W.get(base)
        pp = find_prompt(path)
        md = os.path.join(a.out, parent, stem+'.md')
        try:
            row = analyze_file(path, key, '%s/%s' % (parent, stem), read_text(pp) if pp else '', wins or [], md, want_md=wins is not None)
        except Exception:
            failed += 1; print('FAILED', path, file=sys.stderr); traceback.print_exc(); continue
        rows.append(row)
        text = tsv_text(rows[0]).split('\n')[0]+'\n'+''.join(tsv_text(r).split('\n')[1]+'\n' for r in rows)
        with open(os.path.join(a.out, 'summary.tsv'), 'w') as f: f.write(text)
        print('done', key, flush=True)
    sys.exit(1 if failed else 0)

if __name__ == '__main__':
    main()
