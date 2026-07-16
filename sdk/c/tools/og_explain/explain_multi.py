#!/usr/bin/env python3
# One command -> the MULTI-BOT replay X-ray page (every bot's reasoning).
#
#   python3 sdk/c/tools/og_explain/explain_multi.py <replay-url> <deal-seed> \
#           <octo-seats-csv> [out.html]
#
# Decodes the replay, drives it through the DEPLOYED wasm querying EVERY bot at
# its turns (OG_EXPLAIN deliberation for the octogen seats, legal-move menus for
# the rest), merges, and renders. Nothing about the specific game is hardcoded.
#
# Requirements: node (tsx) + a C compiler. The instrumented wasm is built on
# demand (make bots-wasm-explain), swapped in for the drive, and restored after.
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CNITRO = os.path.abspath(os.path.join(HERE, '..', '..'))
ROOT = os.path.abspath(os.path.join(CNITRO, '..'))


def run(cmd, **kw):
    print('  $ ' + ' '.join(cmd), file=sys.stderr)
    subprocess.run(cmd, check=True, **kw)


def drive_wasm(seed, rd, delib, octo_csv):
    print('[2/4] build OG_EXPLAIN wasm (make bots-wasm-explain)', file=sys.stderr)
    run(['make', '-s', 'bots-wasm-explain'], cwd=CNITRO)
    shipped = os.path.join(ROOT, 'supabase', 'functions', '_shared', 'wasm', 'bots.wasm.gz')
    explain_gz = os.path.join(CNITRO, 'build', 'bots-explain.wasm.gz')
    backup = shipped + '.mbak'
    shutil.copyfile(shipped, backup)
    try:
        shutil.copyfile(explain_gz, shipped)
        print('[3/4] drive every bot through the deployed wasm (OG_EXPLAIN + legal menus)', file=sys.stderr)
        env = dict(os.environ, RECON_SEED=seed, RECON_RD=rd, OGX_MULTI_DELIB=delib,
                   OGX_OCTO_SEATS=octo_csv,
                   TSX_TSCONFIG_PATH=os.path.join(ROOT, 'e2e', 'tsconfig.json'))
        run(['node', '--import', 'tsx', '--test',
             os.path.join(ROOT, 'e2e', '_wasm_multi_drive.test.ts')], env=env, cwd=ROOT)
    finally:
        shutil.copyfile(backup, shipped)   # always restore the committed wasm
        os.remove(backup)


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(argv) < 3:
        print('usage: explain_multi.py <replay-url> <deal-seed> <octo-seats-csv> [out.html]', file=sys.stderr)
        return 2
    url, seed, octo_csv = argv[0], argv[1], argv[2]
    out_html = argv[3] if len(argv) > 3 else os.path.join(ROOT, 'docs', 'octogen-4v4-replay.html')

    work = tempfile.mkdtemp(prefix='ogxm_')
    rd = os.path.join(work, 'replay_decoded.json')
    delib = os.path.join(work, 'delib.jsonl')

    print('[1/4] decode replay URL', file=sys.stderr)
    run(['node', '--import', 'tsx', os.path.join(HERE, 'decode_to_json.mjs'), url, rd])

    drive_wasm(seed, rd, delib, octo_csv)

    print('[4/4] merge + render -> ' + out_html, file=sys.stderr)
    run([sys.executable, os.path.join(HERE, 'multi_page.py'), rd, delib, seed, octo_csv, out_html])

    print('\nwrote ' + out_html, file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
