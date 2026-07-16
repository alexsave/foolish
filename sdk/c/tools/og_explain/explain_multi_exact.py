#!/usr/bin/env python3
# One command -> the DETERMINISTIC multi-bot 4v4 X-ray (every bot's reasoning).
#
#   python3 sdk/c/tools/og_explain/explain_multi_exact.py [gen.json] [out.html]
#
# The game is generated AND replayed entirely through the deployed wasm's own
# decision path (kernel session log / belief_log_bytes), and the X-ray drives the
# EXACT recorded picks — not the lossy replay URL — so octogen reproduces every
# decision byte-for-byte (0 "would differ"). If no gen.json is given (or it's
# missing) one is generated first: a seed-dealt 4-octogen (0-3) vs 4-random (4-7)
# game that an octogen wins.
#
# Requirements: node (tsx) + a C compiler. The instrumented wasm is built on
# demand (make bots-wasm-explain), swapped in for the drive, and restored after.
import json
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


def generate(gen_path):
    print('[1/5] generate a kernel-path 4v4 octogen-win record (shipped wasm)', file=sys.stderr)
    env = dict(os.environ, OGX_GEN_OUT=gen_path, SC_RUN='1', GEN_MAX='150',
               TSX_TSCONFIG_PATH=os.path.join(ROOT, 'e2e', 'tsconfig.json'))
    run(['node', '--import', 'tsx', '--test', os.path.join(ROOT, 'e2e', '_wasm_4v4_gen.test.ts')],
        env=env, cwd=ROOT)
    if not os.path.exists(gen_path):
        raise SystemExit('generation produced no record (no octogen win in budget)')


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith('--')]
    gen_path = argv[0] if len(argv) > 0 else os.path.join(tempfile.mkdtemp(prefix='ogxm_'), 'gen.json')
    out_html = argv[1] if len(argv) > 1 else os.path.join(ROOT, 'docs', 'octogen-4v4-replay.html')

    if not os.path.exists(gen_path):
        generate(gen_path)

    gen = json.load(open(gen_path))
    seed = gen['seed']
    octo_csv = ','.join(str(s) for s in gen['octogenSeats'])
    work = tempfile.mkdtemp(prefix='ogxe_')
    rd_path = os.path.join(work, 'rd.json')
    delib = os.path.join(work, 'delib.jsonl')
    json.dump(gen['rd'], open(rd_path, 'w'))

    print('[2/5] build OG_EXPLAIN wasm (make bots-wasm-explain)', file=sys.stderr)
    run(['make', '-s', 'bots-wasm-explain'], cwd=CNITRO)
    shipped = os.path.join(ROOT, 'supabase', 'functions', '_shared', 'wasm', 'bots.wasm.gz')
    explain_gz = os.path.join(CNITRO, 'build', 'bots-explain.wasm.gz')
    backup = shipped + '.mxbak'
    shutil.copyfile(shipped, backup)
    try:
        shutil.copyfile(explain_gz, shipped)
        print('[3/5] drive the EXACT recorded picks through the deployed wasm', file=sys.stderr)
        env = dict(os.environ, RECON_SEED=seed, OGX_GEN_JSON=gen_path, OGX_MULTI_DELIB=delib,
                   OGX_OCTO_SEATS=octo_csv,
                   TSX_TSCONFIG_PATH=os.path.join(ROOT, 'e2e', 'tsconfig.json'))
        run(['node', '--import', 'tsx', '--test', os.path.join(ROOT, 'e2e', '_wasm_multi_exact.test.ts')],
            env=env, cwd=ROOT)
    finally:
        shutil.copyfile(backup, shipped)   # always restore the committed wasm
        os.remove(backup)

    print('[4/5] merge + render', file=sys.stderr)
    run([sys.executable, os.path.join(HERE, 'multi_page.py'), rd_path, delib, seed, octo_csv, out_html])

    print('[5/5] done', file=sys.stderr)
    print('\nreplay url: ' + gen.get('url', '(none)'), file=sys.stderr)
    print('wrote ' + out_html, file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
