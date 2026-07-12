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


def drive_native(seed, rd, moves, delib):
    """Native og_explain build (fast, but different compile flags than the
    deployed wasm — a 'would differ' flag is native-vs-wasm MC noise)."""
    print('[2/5] build og_explain analysis binary (make og_explain)', file=sys.stderr)
    run(['make', '-s', 'og_explain'], cwd=CNITRO)
    print('[3/5] convert to moves + drive octogen (OG_EXPLAIN)', file=sys.stderr)
    run([sys.executable, os.path.join(HERE, 'make_moves.py'), rd, moves])
    env = dict(os.environ, OG_EXPLAIN=delib, CD_BUDGET='prod', CD_RACE='1', CD_RACE_C='75')
    run([os.path.join(CNITRO, 'build', 'og_explain'), seed, moves],
        env=env, stdout=subprocess.DEVNULL)


def drive_wasm(seed, rd, delib):
    """The DEPLOYED bots.wasm (bots-wasm-explain = shipped wasm + behavior-neutral
    OG_EXPLAIN sink), so the deliberation is the exact binary that played the
    game — no native-vs-wasm divergence. Swaps the instrumented wasm in for the
    drive and restores the committed one after."""
    import shutil
    print('[2/5] build OG_EXPLAIN wasm (make bots-wasm-explain)', file=sys.stderr)
    run(['make', '-s', 'bots-wasm-explain'], cwd=CNITRO)
    shipped = os.path.join(ROOT, 'supabase', 'functions', '_shared', 'wasm', 'bots.wasm.gz')
    explain_gz = os.path.join(CNITRO, 'build', 'bots-explain.wasm.gz')
    backup = shipped + '.ogx-bak'
    shutil.copyfile(shipped, backup)
    try:
        shutil.copyfile(explain_gz, shipped)
        print('[3/5] drive octogen through the deployed wasm (OG_EXPLAIN)', file=sys.stderr)
        env = dict(os.environ, RECON_SEED=seed, RECON_RD=rd, OGX_WASM_DELIB=delib,
                   TSX_TSCONFIG_PATH=os.path.join(ROOT, 'e2e', 'tsconfig.json'))
        # Driver lives in e2e/ so the deal-seed override resolves to one engine.ts
        # module instance (from other dirs it duplicates and the deal goes random).
        run(['node', '--import', 'tsx', '--test',
             os.path.join(ROOT, 'e2e', '_wasm_drive.test.ts')], env=env, cwd=ROOT)
    finally:
        shutil.copyfile(backup, shipped)   # always restore the committed wasm
        os.remove(backup)


def main():
    argv = [a for a in sys.argv[1:] if a != '--wasm']
    use_wasm = '--wasm' in sys.argv
    if len(argv) < 2:
        print('usage: explain.py [--wasm] <replay-url> <deal-seed> [out.html]', file=sys.stderr)
        return 2
    url, seed = argv[0], argv[1]
    out_html = argv[2] if len(argv) > 2 else os.path.join(ROOT, 'docs', 'octogen-replay-explain.html')

    work = tempfile.mkdtemp(prefix='ogx_')
    rd = os.path.join(work, 'replay_decoded.json')
    moves = os.path.join(work, 'moves.txt')
    delib = os.path.join(work, 'delib.jsonl')
    page = os.path.join(work, 'page_data.json')

    print('[1/5] decode replay URL', file=sys.stderr)
    run(['node', '--import', 'tsx', os.path.join(HERE, 'decode_to_json.mjs'), url, rd])

    if use_wasm:
        drive_wasm(seed, rd, delib)
    else:
        drive_native(seed, rd, moves, delib)

    print('[4/5] merge deliberation + replay -> page_data', file=sys.stderr)
    run([sys.executable, os.path.join(HERE, 'build_data.py'), rd, delib, seed, page])

    print('[5/5] render HTML' + ('  (source: DEPLOYED wasm)' if use_wasm else '  (source: native)'), file=sys.stderr)
    run([sys.executable, os.path.join(HERE, 'gen_html.py'), page, out_html])

    print('\nwrote ' + out_html, file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
