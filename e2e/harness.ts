// Tiny harness around the REAL deployed server code. It only does what the
// platform would otherwise do: provide Deno env/globals, a database per test
// file, and a reset. Everything gameplay-related goes through the genuine
// _shared modules (table_io's CAS loop over the C Table, commit_table, the bot
// lease); rows are seeded by the kernel (e2e/helpers/table_db.ts).

// Deno globals the server modules read at import/runtime.
(globalThis as any).Deno = (globalThis as any).Deno || { env: { get: (k: string) => process.env[k] || 'x' } };
(globalThis as any).EdgeRuntime = (globalThis as any).EdgeRuntime || { waitUntil: (_p: Promise<unknown>) => {} };

// The real handlers / bot code log play-by-play; silence the gameplay chatter so
// test output stays readable (assertions still surface failures).
if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

import { readFileSync } from 'fs';
import { basename, join } from 'path';
import { Client } from 'pg';
import { after } from 'node:test';
import { e2ePool as pool, pgAdminConfig, suiteDatabase, resetBroadcastLog } from './adapters/supabase.ts';
import { derivedUuid } from '../sdk/ts/wire/detid.ts';
import { suiteRng } from './helpers/rng.ts';

export { broadcastLog, resetBroadcastLog } from './adapters/supabase.ts';

// Game ids and player ids used to come from crypto.randomUUID(), which made
// every suite that seeds a game a different experiment each run: a red
// "game m4a3f2, player 9c1e… rejected" named nothing anyone could re-run, and
// the fuzzers that advertise a FUZZ_SEED were quietly mixing entropic player
// ids into the stream the seed was supposed to pin.
//
// They are derived instead, from the suite seed and the test FILE. The file is
// in the namespace because node --test runs each file in its own process, so
// without it two files would hand the shared Postgres the same ids; with it,
// one file's ids are stable across runs and disjoint from every other file's.
// The counter guarantees uniqueness inside the file exactly, not probabilistically.
const idRng = suiteRng('ids');
const idNamespace = `${idRng.seed}:${basename(process.argv[1] ?? 'e2e')}`;
let idSeq = 0;

/** A UUID for a test row. Reproducible from E2E_SEED_IDS (or E2E_SEED). */
export const uuid = () => derivedUuid(idNamespace, idSeq++);

/** Reset the id counter - only for a test that wants two identical id runs. */
export const __resetIds = () => { idSeq = 0; };

// One short-lived connection to the maintenance database, for the two statements
// that cannot run against the database they act on. A Client rather than a Pool:
// it is opened, used and closed inside the call, so a file's steady-state
// footprint stays exactly its own pool (see the connection budget in
// adapters/supabase.ts).
async function onAdmin(sql: string): Promise<void> {
    const admin = new Client(pgAdminConfig);
    await admin.connect();
    try { await admin.query(sql); } finally { await admin.end(); }
}

let ownsDatabase = false;

// Create this FILE's database, then stand up the Supabase platform shim and apply
// the REAL production schema (server/impls/supabase/seed.sql — tables, types, the
// commit_game CAS, the bot lease, the triggers) verbatim into it. seed.sql is the
// single source of truth; nothing about the gameplay schema is copied here, and
// nothing about it is rewritten to fit the isolation either - a per-file DATABASE
// (rather than a per-file schema) is what lets `auth.users` and `realtime.messages`
// keep the names production calls them by, so the harness can't drift from
// production.
//
// DROP-then-CREATE, in that order: the DROP is the cleanup for whatever a killed
// run left behind, and WITH (FORCE) terminates its orphaned backends instead of
// failing on them. This is why a stale namespace is inert rather than poisonous.
export async function applySchema(): Promise<void> {
    await applyPlatformShim();
    const seed = readFileSync(join(process.cwd(), 'server', 'impls', 'supabase', 'seed.sql'), 'utf8');
    await pool.query(seed);
}

/**
 * A fresh database for this file with only the Supabase platform shim, no app
 * schema. For a suite that must build the schema some other way than seed.sql
 * (e2e/db_platform_grants.test.ts loads it under Supabase's default
 * privileges). Recreates the database, so calling it again starts over.
 */
export async function applyPlatformShim(): Promise<void> {
    await onAdmin(`DROP DATABASE IF EXISTS ${suiteDatabase} WITH (FORCE)`);
    await onAdmin(`CREATE DATABASE ${suiteDatabase}`);
    ownsDatabase = true;

    const shim = readFileSync(join(process.cwd(), 'e2e', 'schema.sql'), 'utf8');
    await pool.query(shim);
    // And the two platform EXTENSIONS, before seed.sql rather than after it.
    // seed.sql schedules the bot heartbeat and the pg_net response-log VACUUM
    // (its SCHEDULED JOBS section), and it skips them on a Postgres that has no
    // pg_cron - so a database that met the shapes only afterwards would build a
    // schema with no bot loop in it and every assertion about the jobs would be
    // asserting nothing. Standing them up here makes "what seed.sql builds" the
    // same question in the harness as it is on a server.
    const extensions = readFileSync(join(process.cwd(), 'e2e', 'fixtures', 'platform_extensions.sql'), 'utf8');
    await pool.query(extensions);
}

// Release: close the pool, then drop the file's database. Best-effort by design:
// a run killed between here and the next applySchema() leaves at most one inert
// database per test file, which the next run's DROP ... WITH (FORCE) removes.
let tornDown: Promise<void> | null = null;
export function teardownSuiteDb(): Promise<void> {
    return (tornDown ??= (async () => {
        await pool.end();
        if (!ownsDatabase) return;
        try { await onAdmin(`DROP DATABASE IF EXISTS ${suiteDatabase} WITH (FORCE)`); } catch { /* the next run's DROP gets it */ }
    })());
}

// Under `node --test` each file is its own process and its own root suite, so a
// root after() here runs once, last, for every suite that imports the harness,
// including the ones that end the pool themselves (pool.end() folds repeat calls).
// Outside the runner (the e2e/bench_*.ts scripts) there is no root suite, so hang
// the same teardown off beforeExit instead.
if (process.env.NODE_TEST_CONTEXT) after(async () => { await teardownSuiteDb(); });
else process.on('beforeExit', () => { void teardownSuiteDb(); });

export async function resetDb(): Promise<void> {
    await pool.query('TRUNCATE games, player_hands, bot_hands, bots, game_snapshots, user_elo_ratings, player_views RESTART IDENTITY CASCADE');
    await pool.query('TRUNCATE auth.users CASCADE');
    resetBroadcastLog();
}

// Seeding a game is the kernel's: e2e/helpers/table_db.ts seedTable writes a
// C-built fixture as a kernel-owned row, and e2e/helpers/table_server.ts
// seedLobby a lobby of given seats (docs/C_GAME_SHAPE_MIGRATION.md Phase 4b).

export const pgPool = pool;
