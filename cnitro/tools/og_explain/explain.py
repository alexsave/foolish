#!/usr/bin/env python3
# One command, one game -> the octogen deliberation X-ray page.
#
#   python3 cnitro/tools/og_explain/explain.py <replay-url> <deal-seed> [out.html]
#
# Fully automated: decode the replay, drive it through the engine querying
# octogen at every one of its turns (OG_EXPLAIN dump), merge, render. Nothing
# about the specific game is hardcoded anywhere in the pipeline -- pass a
# different (url, seed) and you get a correct page for that game.
#
# Requirements: node (with tsx, for the TS replay codec) and a C compiler.
# The octogen analysis binary is built on demand via `make og_explain`.
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CNITRO = os.path.abspath(os.path.join(HERE, '..', '..'))
ROOT = os.path.abspath(os.path.join(CNITRO, '..'))


def run(cmd, **kw):
    print('  $ ' + ' '.join(cmd), file=sys.stderr)
    subprocess.run(cmd, check=True, **kw)


def main():
    if len(sys.argv) < 3:
        print('usage: explain.py <replay-url> <deal-seed> [out.html]', file=sys.stderr)
        return 2
    url, seed = sys.argv[1], sys.argv[2]
    out_html = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, 'docs', 'octogen-replay-explain.html')

    work = tempfile.mkdtemp(prefix='ogx_')
    rd = os.path.join(work, 'replay_decoded.json')
    moves = os.path.join(work, 'moves.txt')
    delib = os.path.join(work, 'delib.jsonl')
    page = os.path.join(work, 'page_data.json')

    print('[1/5] decode replay URL', file=sys.stderr)
    run(['node', '--import', 'tsx', os.path.join(HERE, 'decode_to_json.mjs'), url, rd])

    print('[2/5] build og_explain analysis binary (make og_explain)', file=sys.stderr)
    run(['make', '-s', 'og_explain'], cwd=CNITRO)

    print('[3/5] convert to moves + drive octogen (OG_EXPLAIN)', file=sys.stderr)
    run([sys.executable, os.path.join(HERE, 'make_moves.py'), rd, moves])
    env = dict(os.environ, OG_EXPLAIN=delib, CD_BUDGET='prod', CD_RACE='1', CD_RACE_C='75')
    run([os.path.join(CNITRO, 'build', 'og_explain'), seed, moves],
        env=env, stdout=subprocess.DEVNULL)

    print('[4/5] merge deliberation + replay -> page_data', file=sys.stderr)
    run([sys.executable, os.path.join(HERE, 'build_data.py'), rd, delib, seed, page])

    print('[5/5] render HTML', file=sys.stderr)
    run([sys.executable, os.path.join(HERE, 'gen_html.py'), page, out_html])

    print('\nwrote ' + out_html, file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
