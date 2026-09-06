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
 *   db   - the ~21 files that import e2e/harness.ts. Bounded not by CPU but by
 *          Postgres' default max_connections of 100 (local dev and the CI
 *          postgres:16 service alike, 3 of them reserved for superusers). The
 *          per-file pool sizes and the arithmetic that fixes this lane's width
 *          are documented in e2e/adapters/supabase.ts; the short version is
 *          24 + 30 + 8 + 8 + 4 admin = 74 worst case at a width of 4.
 *
 * Membership is derived from the import, not from a list anyone has to maintain:
 * harness.ts is the only door to the pool, so importing it is exactly the
 * property "this file may open Postgres connections".
 * ========================================================================== */

import { spawn } from 'node:child_process';
import { readdirSync, readFileSync } from 'node:fs';
import { availableParallelism } from 'node:os';
import { join } from 'node:path';

const E2E_DIR = 'e2e';

// `--exclude <basename>` may be repeated. coverage:e2e uses it for
// lobby_add_bot.test.ts (a jsdom React render that is flaky headless).
const argv = process.argv.slice(2);
const excluded = new Set();
for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--exclude') excluded.add(argv[++i]);
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

const pureWidth = Number(process.env.E2E_CONCURRENCY || availableParallelism());
const dbWidth = Number(process.env.E2E_DB_CONCURRENCY || 4);

// 'overlap' (default) runs both lanes at once - the db lane spends most of its
// time waiting on Postgres, so it costs the pure lane little. 'serial' runs them
// one after the other, which is what to reach for when a red log is unreadable.
const overlap = (process.env.E2E_LANES || 'overlap') === 'overlap';

function lane(name, laneFiles, width, buffered) {
    if (laneFiles.length === 0) return Promise.resolve(0);
    const child = spawn(process.execPath, [
        '--import', 'tsx', '--test', `--test-concurrency=${width}`,
        '--experimental-test-module-mocks', ...laneFiles,
    ], {
        env: { ...process.env, TSX_TSCONFIG_PATH: 'e2e/tsconfig.json' },
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

console.error(`[e2e] pure lane: ${pureFiles.length} files x${pureWidth} | db lane: ${dbFiles.length} files x${dbWidth} | lanes ${overlap ? 'overlapped' : 'serial'}`);

const codes = overlap
    ? await Promise.all([lane('pure', pureFiles, pureWidth, false), lane('db', dbFiles, dbWidth, true)])
    : [await lane('pure', pureFiles, pureWidth, false), await lane('db', dbFiles, dbWidth, false)];

process.exit(codes.some((c) => c !== 0) ? 1 : 0);
