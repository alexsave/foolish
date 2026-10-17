#!/usr/bin/env node
/* =============================================================================
 * The e2e runner: one lane, the most expensive files first
 * =============================================================================
 * Every file is pure (codec, kernel, client reconciliation, the page in jsdom):
 * none opens a database. The Postgres lane, and the harness that gave each of
 * its files a database of its own, went with the TypeScript server in stage 4
 * of c_base/DESIGN.md "Binary surface"; the server is C and its tests are
 * c_base's. So the files run at the machine's parallelism.
 *
 * The ORDER they start in is e2e/FILE_ORDER - a generated, plain-text list,
 * most expensive first, refreshed by `node scripts/run_e2e.mjs --write-order` -
 * because node:test starts files in list order as slots free, and a long file
 * started last decides the makespan (the longest-processing-time rule). It is a
 * HINT and nothing else: a file missing from it runs first (pessimistic: an
 * unmeasured file may be the long one), a file in it that no longer exists is
 * ignored, and a stale order costs wall-clock and can never cost correctness.
 * The key is CPU time, not elapsed: see scripts/e2e_order_probe.mjs.
 * ========================================================================== */

import { execFileSync, spawn } from 'node:child_process';
import { existsSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { availableParallelism, tmpdir } from 'node:os';
import { basename, join } from 'node:path';
import { fileURLToPath } from 'node:url';

// Inherited by every per-file child node:test spawns. It keeps the test's own
// prints off fd 1, which in a child is node's v8-serialized report wire - see
// scripts/e2e_child_stdout.mjs for what the parent does with a print it finds
// wedged between two frames.
const STDOUT_GUARD = fileURLToPath(new URL('./e2e_child_stdout.mjs', import.meta.url));

// Preloaded only under --write-order. It asserts nothing and prints nothing; it
// records what the run it rides along with measured, so the next run can start
// the expensive files first.
const ORDER_PROBE = fileURLToPath(new URL('./e2e_order_probe.mjs', import.meta.url));

const E2E_DIR = 'e2e';
const ORDER_FILE = join(E2E_DIR, 'FILE_ORDER');

// `--exclude <basename>` may be repeated. coverage:e2e uses it for
// lobby_add_bot.test.ts (a jsdom React render that is flaky headless).
// `--write-order` regenerates e2e/FILE_ORDER from this run's own measurements.
const argv = process.argv.slice(2);
const excluded = new Set();
let writeOrder = false;
for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--exclude') excluded.add(argv[++i]);
    else if (argv[i] === '--write-order') writeOrder = true;
    else throw new Error(`run_e2e: unknown argument ${argv[i]}`);
}

const files = readdirSync(E2E_DIR)
    .filter((f) => f.endsWith('.test.ts') && !excluded.has(f))
    .sort()
    .map((f) => join(E2E_DIR, f));

// e2e/FILE_ORDER, most expensive first: `<basename>\t<cpu_s>\t<elapsed_s>` per line,
// `#` comments ignored. Only the ORDER is read - the seconds are there for a
// human, and trusting them numerically would invite treating a hint as a budget.
// A file this list has never seen is ranked ahead of every file it has, because
// the cost of guessing low on a long file is the whole run's makespan and the
// cost of guessing high on a short one is one slot for a second.
const ranked = new Map(
    (existsSync(ORDER_FILE) ? readFileSync(ORDER_FILE, 'utf8') : '')
        .split('\n').map((l) => l.trim()).filter((l) => l && !l.startsWith('#'))
        .map((line, i) => [line.split(/\s+/)[0], i]));
const slowestFirst = (list) =>
    [...list].sort((a, b) => (ranked.get(basename(a)) ?? -1) - (ranked.get(basename(b)) ?? -1));
const unranked = files.filter((f) => !ranked.has(basename(f))).length;

const width = Number(process.env.E2E_CONCURRENCY || availableParallelism());

// --write-order collects the run's measurements here.
const orderDir = writeOrder ? mkdtempSync(join(tmpdir(), 'e2e-order-')) : null;
const orderOut = orderDir ? join(orderDir, 'times') : null;

const probe = writeOrder ? ['--import', ORDER_PROBE] : [];

// The native table host (c/tests/table_host.c), the server half of a game for
// the files that hold shipped TypeScript: built once here, before any file
// starts, and named to every child in TABLE_HOST, so no file pays for a make
// and none races another to run one (e2e/helpers/table_host.ts builds it itself
// only when a file is run on its own). An explicit TABLE_HOST is kept.
const tableHost = process.env.TABLE_HOST ?? fileURLToPath(new URL('../c/build/table_host', import.meta.url));
if (!process.env.TABLE_HOST) {
    execFileSync('make', ['-s', '-C', fileURLToPath(new URL('../c', import.meta.url)), 'table-host'], { stdio: ['ignore', 'ignore', 'inherit'] });
}
// Printed rather than swallowed: it is how a reader finds out that
// e2e/FILE_ORDER has drifted behind the tree.
console.error(`[e2e] ${files.length} files x${width} | slowest first, ${unranked} file(s) unranked`);
const code = await new Promise((resolve) => spawn(process.execPath, [
    '--import', 'tsx', '--import', STDOUT_GUARD, ...probe,
    '--test', `--test-concurrency=${width}`,
    '--experimental-test-module-mocks', ...slowestFirst(files),
], {
    env: { ...process.env, TSX_TSCONFIG_PATH: 'e2e/tsconfig.json', TABLE_HOST: tableHost, ...(orderOut ? { E2E_ORDER_OUT: orderOut } : {}) },
    stdio: 'inherit',
}).on('close', (c) => resolve(c ?? 1)));

if (orderOut) {
    // Each child appended `<path> <elapsed_ms> <cpu_ms>`. Rank on CPU, with
    // elapsed kept beside it; scripts/e2e_order_probe.mjs explains why cpu is
    // the key. Basenames only: the prefix would be a hundred copies of "e2e/".
    const rows = (existsSync(orderOut) ? readFileSync(orderOut, 'utf8') : '')
        .split('\n').filter(Boolean)
        .map((l) => { const p = l.split(' '); return [basename(p[0]), Number(p[1]), Number(p[2])]; })
        .sort((a, b) => b[2] - a[2]);
    writeFileSync(ORDER_FILE,
        '# The e2e files, most expensive first, by CPU time (user + system) inside the\n'
        + '# per-file child. Generated by `node scripts/run_e2e.mjs --write-order`; do not\n'
        + '# hand-edit. The runner starts the files in this order so a long file\n'
        + '# cannot start last - see scripts/run_e2e.mjs for the measurement that motivated\n'
        + '# it, and scripts/e2e_order_probe.mjs for why the key is cpu and not elapsed.\n'
        + '#\n'
        + '# A HINT and nothing more: an unlisted file runs first, a listed one that is gone\n'
        + '# is ignored, and a stale list costs wall-clock and can never cost correctness.\n'
        + '#\n'
        + '# file\tcpu_s\telapsed_s   (a file whose elapsed dwarfs its cpu is waiting, not working)\n'
        + rows.map(([f, ms, cpu]) => `${f}\t${(cpu / 1000).toFixed(1)}\t${(ms / 1000).toFixed(1)}`).join('\n') + '\n');
    rmSync(orderDir, { recursive: true, force: true });
    console.error(`[e2e] wrote ${ORDER_FILE} (${rows.length} files)`);
}

process.exit(code === 0 ? 0 : 1);
