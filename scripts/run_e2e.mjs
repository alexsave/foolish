#!/usr/bin/env node
/* =============================================================================
 * The e2e runner: two lanes, sized by what each one can actually contend for
 * =============================================================================
 * The suite used to run `--test-concurrency=1` - 81 files, strictly one at a
 * time, 27 minutes on the critical path of every PR. The reason was never
 * gameplay. concurrent_games.test.ts exists precisely to show that 24 real games
 * on ONE Postgres neither deadlock nor corrupt each other. What could not run in
 * parallel was the harness's own reset: every Postgres-backed file DROPped and
 * recreated the public/auth/realtime schemas of the single shared database, out
 * from under whatever else was mid-transaction.
 *
 * e2e/adapters/supabase.ts removed that shared thing: each file now owns a
 * database named after itself. Two lanes fall out of it.
 *
 *   pure - the ~60 files that never import the harness (codec, kernel, client
 *          reconciliation, parity). No Postgres, no connections, nothing to
 *          contend for. Runs at the machine's parallelism.
 *
 *   db   - the ~37 files that import e2e/harness.ts. Bounded not by CPU but by
 *          Postgres' default max_connections of 100 (local dev and the CI
 *          postgres:16 service alike, 3 of them reserved for superusers). The
 *          per-file pool sizes and the arithmetic that fixes this lane's width
 *          are documented in e2e/adapters/supabase.ts; the short version is
 *          24 + 30 + 8 + 8 + 8 + 5 admin = 83 worst case at a width of 5.
 *
 * Membership is derived from the import, not from a list anyone has to maintain:
 * harness.ts is the only door to the pool, so importing it is exactly the
 * property "this file may open Postgres connections".
 *
 * ---------------------------------------------------------------------------
 * WHAT IS LEFT AFTER THE LOADER FIX: the ORDER the files are started in
 * ---------------------------------------------------------------------------
 * Both lanes hand node:test a list and node starts files in that order as slots
 * free. The list was alphabetical, and that is a scheduling decision nobody made
 * on purpose. Over a full run: 110 files, 818s of summed per-file wall clock,
 * the longest single file 80s - and security_hidden_info.test.ts, that 80s file,
 * sorts under "s", so the db lane started it when it was two thirds done and
 * then had nothing to overlap it with. Same for state_codec (73s, "s") in the
 * pure lane. Starting the long files first is the textbook longest-processing-
 * time rule, and nothing about any test changes - only when it is started.
 *
 * So the runner reads e2e/FILE_ORDER - a generated, plain-text list, most
 * expensive first, refreshed by `node scripts/run_e2e.mjs --write-order` - and
 * starts each lane's files in it. It is a HINT and nothing else: a file missing
 * from it runs first (pessimistic - an unmeasured file may be the next 80s one),
 * a file in it that no longer exists is ignored, and a stale order costs
 * makespan and can never cost correctness. It is a measured fact written down by
 * the tool that measured it, never by hand - the same bargain
 * sdk/ts/wasm/WASM_STAMP used to make before the shipped modules stopped being
 * committed and it was deleted with them. The key is CPU time, not elapsed - see
 * scripts/e2e_order_probe.mjs for why.
 *
 * The width moves 4 -> 5 with it, and stops there. Wider was tried properly,
 * not waved away: every db file's peak pooled-connection count was measured
 * (31 of the 37 peak at 2 connections, and only lease, concurrent_games,
 * pool_teardown and adversarial_ts_layer need room), the ordinary pool was cut
 * from 8 to 4 on that evidence, and the connection budget then allows a width of
 * 8. The db lane ran 68.5s at width 8 against 56.3s at width 5. Slots were never
 * what it was short of - one Postgres and eight cores were - so the pool sizes
 * stay as they were and the only thing kept from that experiment is this
 * paragraph, so nobody spends the afternoon on it again.
 *
 * Measured back to back on an 8-core M-series Mac, full `node scripts/run_e2e.mjs`,
 * 542 tests passing at every row:
 *
 *     before, alphabetical, db x4                         124.1s
 *     expensive-first, db x5, synchronous_commit=off       95.6s
 *     + security_hidden_info split into three files        79.2s
 *     + belief_logs and state_codec ported to c/tests      72.5s / 74.3s
 *
 * Scheduling is the smallest of those, and the honest reason is worth writing
 * down: a queue-simulation over the per-file times predicted 80s for the second
 * row and was wrong, because per-file times are not constants. Front-loading the
 * long files makes them all run AT ONCE, so each gets less machine and takes
 * longer. Ordering can only ever move the tail; it cannot move the floor.
 *
 * The floor is one file, and the only thing that ever moved it was making that
 * file cheaper or making it into several files. Every other lever was measured
 * and refused: a wider db lane (68.5s against 56.3s, above), `nice`-ing the pure
 * lane (107.0s against 108.5s), and turning off the per-COMMIT fsync, which is
 * real but worth 3.4s of the db lane's 59.7s rather than the 5-10x a write-bound
 * workload would see. The db lane is not write-bound: it is 191,198 Postgres
 * round trips and 86s of in-query time against 73s of node CPU.
 *
 * Every row above was measured against a Homebrew PostgreSQL 15 that was
 * silently winning :5432 from the postgres:16 container the README and all
 * three workflows name - a specific 127.0.0.1 bind beats a container's wildcard
 * one. e2e/harness.ts now asserts the major version before it creates the first
 * database, so that cannot recur, and it turns the server's durability off
 * (ALTER SYSTEM fsync/full_page_writes/synchronous_commit + reload, the only
 * mechanism a CI `services:` block can also get): 50.5s -> 46.1s on the db lane
 * alone, 87.7s -> 83.2s over one adjacent block of whole-suite runs (only
 * adjacent runs compare on a shared machine). Postmaster-level settings
 * (wal_level=minimal, shared_buffers) and a tmpfs data directory were measured
 * on top of that and are worth nothing; e2e/README.md has the table.
 *
 * Which leaves the biggest number in the local suite being the container
 * itself: one warm connection, 20,000 SELECT 1, is 40.5us a round trip to a
 * native Postgres and 165us to the same image published through Docker
 * Desktop's port forwarder. Times 191,198 queries that is ~24s of latency, and
 * it is a Mac tax - a Linux CI runner publishes the port into its own network
 * stack with no VM in between.
 *
 * Run the lanes one at a time and the pure lane alone took 67.1s and the db lane
 * alone 56.3s, against 107s together. Overlapping them is still worth having -
 * it beats the 123s of running them back to back - but the two lanes do starve
 * each other, and no width setting fixed it.
 * ========================================================================== */

import { spawn } from 'node:child_process';
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

// The import, not a mention of the word in a comment - a dozen files discuss the
// harness without touching it.
const IMPORTS_HARNESS = /(?:^|\n)\s*import\s[^\n]*['"][^'"]*\/harness\.ts['"]/;
const dbFiles = files.filter((f) => IMPORTS_HARNESS.test(readFileSync(f, 'utf8')));
const pureFiles = files.filter((f) => !dbFiles.includes(f));

// e2e/FILE_ORDER, most expensive first: `<basename>\t<cpu_s>\t<elapsed_s>` per line,
// `#` comments ignored. Only the ORDER is read - the seconds are there for a
// human, and trusting them numerically would invite treating a hint as a budget.
// A file this list has never seen is ranked ahead of every file it has, because
// the cost of guessing low on a long file is the whole lane's makespan and the
// cost of guessing high on a short one is one slot for a second.
const ranked = new Map(
    (existsSync(ORDER_FILE) ? readFileSync(ORDER_FILE, 'utf8') : '')
        .split('\n').map((l) => l.trim()).filter((l) => l && !l.startsWith('#'))
        .map((line, i) => [line.split(/\s+/)[0], i]));
const slowestFirst = (laneFiles) =>
    [...laneFiles].sort((a, b) => (ranked.get(basename(a)) ?? -1) - (ranked.get(basename(b)) ?? -1));
const unranked = files.filter((f) => !ranked.has(basename(f))).length;

const pureWidth = Number(process.env.E2E_CONCURRENCY || availableParallelism());
const dbWidth = Number(process.env.E2E_DB_CONCURRENCY || 5);

// --write-order collects both lanes' measurements here, then merges them.
const orderDir = writeOrder ? mkdtempSync(join(tmpdir(), 'e2e-order-')) : null;
const orderOut = orderDir ? join(orderDir, 'times') : null;

// 'overlap' (default) runs both lanes at once - the db lane spends most of its
// time waiting on Postgres, so it costs the pure lane little. 'serial' runs them
// one after the other, which is what to reach for when a red log is unreadable.
const overlap = (process.env.E2E_LANES || 'overlap') === 'overlap';

function lane(name, laneFiles, width, buffered) {
    if (laneFiles.length === 0) return Promise.resolve(0);
    const probe = writeOrder ? ['--import', ORDER_PROBE] : [];
    const child = spawn(process.execPath, [
        '--import', 'tsx', '--import', STDOUT_GUARD, ...probe,
        '--test', `--test-concurrency=${width}`,
        '--experimental-test-module-mocks', ...laneFiles,
    ], {
        env: { ...process.env, TSX_TSCONFIG_PATH: 'e2e/tsconfig.json', ...(orderOut ? { E2E_ORDER_OUT: orderOut } : {}) },
        stdio: buffered ? ['inherit', 'pipe', 'pipe'] : 'inherit',
    });
    const chunks = [];
    if (buffered) { child.stdout.on('data', (c) => chunks.push(c)); child.stderr.on('data', (c) => chunks.push(c)); }
    return new Promise((resolve) => child.on('close', (code) => {
        if (buffered) {
            process.stdout.write(`\n${'='.repeat(70)}\n=== ${name} lane (${laneFiles.length} files, concurrency ${width})\n${'='.repeat(70)}\n`);
            process.stdout.write(Buffer.concat(chunks));
        }
        resolve(code ?? 1);
    }));
}

// The unranked count is printed rather than swallowed: it is how a reader finds
// out that e2e/FILE_ORDER has drifted behind the tree, which shows up as a lane
// that finishes later than it should and nowhere else.
console.error(`[e2e] pure lane: ${pureFiles.length} files x${pureWidth} | db lane: ${dbFiles.length} files x${dbWidth}`
    + ` | lanes ${overlap ? 'overlapped' : 'serial'} | slowest first, ${unranked} file(s) unranked`);

const pure = slowestFirst(pureFiles);
const db = slowestFirst(dbFiles);
// Not tried again: running the pure lane at `nice -n 10`, on the theory that the
// db lane is latency-bound and the pure lane's 8 CPU-bound children were pushing
// its round trips behind the scheduler. Measured 107.0s against 108.5s - inside
// the noise, so the machinery came back out.
const codes = overlap
    ? await Promise.all([lane('pure', pure, pureWidth, false), lane('db', db, dbWidth, true)])
    : [await lane('pure', pure, pureWidth, false), await lane('db', db, dbWidth, false)];

if (orderOut) {
    // Both lanes' children appended `<path> <elapsed_ms> <cpu_ms>`. Merge and
    // rank on CPU, with elapsed kept beside it; scripts/e2e_order_probe.mjs
    // explains why cpu is the key. Basenames only: the lane a file belongs to
    // is derived from its import, and the prefix would be 110 copies of "e2e/".
    const rows = (existsSync(orderOut) ? readFileSync(orderOut, 'utf8') : '')
        .split('\n').filter(Boolean)
        .map((l) => { const p = l.split(' '); return [basename(p[0]), Number(p[1]), Number(p[2])]; })
        .sort((a, b) => b[2] - a[2]);
    writeFileSync(ORDER_FILE,
        '# The e2e files, most expensive first, by CPU time (user + system) inside the\n'
        + '# per-file child. Generated by `node scripts/run_e2e.mjs --write-order`; do not\n'
        + '# hand-edit. The runner starts each lane\'s files in this order so a long file\n'
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

process.exit(codes.some((c) => c !== 0) ? 1 : 0);
