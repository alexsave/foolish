# assemble.py - writes a replica directory's score.txt and parts.txt from extract.py's events and manifests.
#
# What it does: score.txt = TEMPO, one SECTION of --bars bars, then every part's events file (its first line "# NAME gain_db X"
#   is a comment to the score reader and gives the part's gain); parts.txt = one PART line per part with every manifest
#   (MANIFEST, MANIFEST.2, ... when extract.py split the roots), gain_db from that comment and release 0.012.
# Inputs: --dir DIR --bpm B --bars N --part NAME EVENTS MANIFEST GROUP (repeatable). --manifest-names copies each manifest into
#   DIR as NAME.txt and refers to it by that file name (the replica directory then holds its own manifests).
# How to run: python3 -I assemble.py --dir d --bpm 160 --bars 28 --part DRUMS drums_events.txt drums_man.txt drums
# Known failure modes: none beyond missing files, which stop the run.
import argparse, os, shutil, sys


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', required=True)
    ap.add_argument('--bpm', type=float, required=True)
    ap.add_argument('--bars', type=int, required=True)
    ap.add_argument('--section', default='WINDOW')
    ap.add_argument('--part', nargs=4, action='append', metavar=('NAME', 'EVENTS', 'MANIFEST', 'GROUP'), required=True)
    ap.add_argument('--manifest-names', action='store_true')
    ap.add_argument('--release', type=float, default=0.012)
    a = ap.parse_args(argv)
    os.makedirs(a.dir, exist_ok=True)
    score = ['TEMPO %g' % a.bpm, 'SECTION %s BARS %d KEY C' % (a.section, a.bars)]
    parts = ['# one line per part; see replica/play.c']
    for name, ev, man, group in a.part:
        lines = open(ev).read().splitlines()
        first = lines[0].split()
        if len(first) != 4 or first[0] != '#' or first[2] != 'gain_db':
            sys.exit('assemble: %s must start with "# NAME gain_db X"' % ev)
        gain = float(first[3])
        score += lines
        mans = [man] + [m for m in ('%s.%d' % (man, k) for k in range(2, 17)) if os.path.exists(m)]
        refs = []
        for k, m in enumerate(mans):
            if a.manifest_names:
                dst = os.path.join(a.dir, '%s%s.txt' % (name.lower(), '' if k == 0 else '.%d' % (k + 1)))
                shutil.copyfile(m, dst)
                refs.append(os.path.basename(dst))
            else:
                refs.append(os.path.abspath(m))
        parts.append('PART %s %s gain_db %.4f release_s %g group %s' % (name, ' '.join('sampler ' + r for r in refs), gain, a.release, group))
    open(os.path.join(a.dir, 'score.txt'), 'w').write('\n'.join(score) + '\n')
    open(os.path.join(a.dir, 'parts.txt'), 'w').write('\n'.join(parts) + '\n')


if __name__ == '__main__':
    main()
