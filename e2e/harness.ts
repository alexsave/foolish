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

// ---- The server this suite is allowed to talk to, and how fast it is ------
//
// THE MAJOR VERSION IS AN ASSERTION, because it was silently wrong for months.
// A developer Mac with `brew services start postgresql@15` running AND the
// README's `foolish-e2e-pg` container running has TWO servers on 5432: brew
// binds 127.0.0.1 and [::1] specifically, the container's published port binds
// `*`, and a specific bind beats a wildcard one. So every connection went to
// PostgreSQL 15 (Homebrew) while this README and all three CI workflows said
// postgres:16, and every local timing in the repo had been taken on a server
// nobody meant to measure. `lsof -nP -iTCP:5432 -sTCP:LISTEN` shows both.
//
// A wrong server must therefore be an error and not a 20% difference in
// everyone's numbers. E2E_PG_MAJOR overrides the expectation, for a deliberate
// experiment on another major; it is the one place to change when CI's image
// moves, and CI going red is the point - the workflows and this constant cannot
// drift apart in silence.
const EXPECT_PG_MAJOR = Number(process.env.E2E_PG_MAJOR || 16);

// And the durability a database that is created at a file's start and dropped at
// its end does not need. The pool already asks for synchronous_commit=off per
// session (SESSION_OPTIONS in adapters/supabase.ts); what a session option
// cannot reach is CREATE DATABASE's own checkpoint, the WAL writer, and the
// full-page image written after every checkpoint. Those are server settings, and
// these three are all SIGHUP-settable, which is what makes ALTER SYSTEM + reload
// the one mechanism that works identically here and on a GitHub Actions service
// container - `services:` takes no command arguments, so `-c fsync=off` is a
// thing only a local `docker run` can pass. It runs from the harness so a fresh
// clone and CI both get it with nothing to remember.
//
// Measured on the `postgres:16` container, db lane alone (E2E_LANES=serial),
// on top of the per-session synchronous_commit=off that was already there:
//
//     server as shipped              50.5s
//     these three                    46.6s / 46.1s
//     + wal_level=minimal, max_wal_senders=0, shared_buffers=512MB,
//       max_wal_size=2GB (postmaster settings, needs a restart)    45.7s
//     + the data directory on a tmpfs ramdisk                      46.1s
//
// So the three below are the whole win and the other two rows are noise, which
// is why neither is here: the postmaster settings cannot be set at all on a CI
// service container, and the ramdisk measured at nothing. Whole suite,
// overlapped lanes, 542 tests, six runs in one adjacent block: 87.7s mean
// before, 83.2s after. Only adjacent runs compare - the same tuned suite on a
// quiet machine an hour later was 77.8s.
//
// This is not a weaker database. None of the three changes visibility,
// isolation, locking or any constraint - only when writes reach the platter - so
// every CAS race, deadlock and card-conservation assertion is the same
// experiment it was. What is given up is surviving a power cut, of a database
// whose every row exists to be asserted about once. They are a TEST-database
// choice and must never appear near a real one, which is why the tuning is
// skipped unless the server is on this machine: a loopback host is the cheap
// test for "a server this checkout stood up", and E2E_PGHOST pointing somewhere
// else is exactly the case where writing to postgresql.auto.conf would be wrong.
const FAST_AND_UNDURABLE = ['fsync', 'full_page_writes', 'synchronous_commit'];
const LOCAL_HOSTS = ['127.0.0.1', 'localhost', '::1'];

let serverReady: Promise<void> | null = null;
async function assertAndTuneServer(): Promise<void> {
    const admin = new Client(pgAdminConfig);
    await admin.connect();
    try {
        const { rows } = await admin.query<{ version: string; num: string; name: string; setting: string }>(
            `SELECT version() AS version, current_setting('server_version_num') AS num, name, setting
               FROM pg_settings WHERE name = ANY($1)`, [FAST_AND_UNDURABLE]);
        const major = Math.floor(Number(rows[0]?.num ?? 0) / 10000);
        const where = `${pgAdminConfig.host}:${pgAdminConfig.port}`;
        if (major !== EXPECT_PG_MAJOR) {
            throw new Error(
                `e2e: ${where} is PostgreSQL ${major}, and this suite runs on ${EXPECT_PG_MAJOR}.\n`
                + `  ${rows[0]?.version ?? '(no version)'}\n`
                + '  CI runs the suite against a postgres:16 service (.github/workflows/'
                + 'validate.yml, coverage.yml, metrics.yml), so a local run on another major\n'
                + '  measures and proves something CI never runs.\n'
                + `  Who is on the port:   lsof -nP -iTCP:${pgAdminConfig.port} -sTCP:LISTEN\n`
                + '  A brew server wins 127.0.0.1 over a container published on *; stop it with\n'
                + '                        brew services stop postgresql@15\n'
                + '  Or keep both and give the container its own port:\n'
                + '                        docker run ... -p 55432:5432 postgres:16   (see e2e/README.md)\n'
                + '                        E2E_PGPORT=55432 npm run test:e2e\n'
                + '  To run on another major on purpose:  E2E_PG_MAJOR=' + major);
        }
        if (!LOCAL_HOSTS.includes(String(pgAdminConfig.host))) return;
        if (rows.every((r) => r.setting === 'off')) return;   // already tuned; the usual path
        for (const name of FAST_AND_UNDURABLE) await admin.query(`ALTER SYSTEM SET ${name} = off`);
        await admin.query('SELECT pg_reload_conf()');
    } catch (e) {
        // A version mismatch is fatal. A refused ALTER SYSTEM (a non-superuser
        // role, a managed server) is not: it costs seconds, never correctness,
        // and the suite must still run. Say so once rather than silently.
        if (e instanceof Error && e.message.startsWith('e2e: ')) throw e;
        process.stderr.write(`[e2e] could not tune ${pgAdminConfig.host}:${pgAdminConfig.port}: ${e}\n`);
    } finally { await admin.end(); }
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
    // Before the first statement of the first database, once per process: the
    // right server, and a fast one. Here rather than in scripts/run_e2e.mjs
    // because this is the door every Postgres-backed file comes through, runner
    // or not - `node --test e2e/server.test.ts` on its own gets the same check.
    await (serverReady ??= assertAndTuneServer());
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
