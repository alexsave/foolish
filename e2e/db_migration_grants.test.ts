// E2E: the HOSTED database - the one the migrations build, not the one seed.sql
// builds - exposes no SECURITY DEFINER RPC and no table write to a client role.
//
// e2e/db_grants.test.ts asserts the same function invariant, but over seed.sql.
// The hosted project never runs seed.sql; it receives the migrations. The two
// drifted: 20260906120000 DROPped commit_game and CREATEd it again with a new
// signature. A new function gets EXECUTE for PUBLIC from Postgres and for anon
// and authenticated from Supabase's default privileges, and that migration did
// not repeat 20260807120000's lockdown, so on the hosted project anyone holding
// the public anon key could call commit_game through PostgREST and rewrite any
// game. seed.sql still ran the lockdown after its own CREATE, so db_grants.test.ts
// stayed green the whole time. CREATE OR REPLACE keeps a function's grants;
// DROP + CREATE does not, and nothing but the database can tell them apart.
//
// So this builds what the hosted project went through. The migrations cannot
// build a database from scratch (the baseline is an empty placeholder), so it
// starts from a frozen copy of the schema as of the last migration before
// 20260807120000 (e2e/fixtures/hosted_schema_pre_20260807120000.sql), loads it
// under Supabase's default privileges, and replays every migration from
// 20260807120000 on, in order, exactly as `supabase db push` would. A future
// migration that drops and recreates a definer function without relocking it
// fails here. So does a future table that clients could write.
//
// Then it builds seed.sql under the same platform defaults (the fresh-database
// and `supabase start` path) and asserts the two security postures are
// identical, since the drift between them is how the hole shipped.
//
// Owns the scenarios; the fast runner (e2e/validation/migration_grants_validation.test.ts)
// imports `registerMigrationGrantsValidation`.

import './harness.ts';
import { describe, test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import type { PoolClient } from 'pg';
import { applyPlatformShim, pgPool } from './harness.ts';

const SUPABASE = join(process.cwd(), 'server', 'impls', 'supabase');
const MIGRATIONS = join(SUPABASE, 'migrations');
const FIXTURE = join(process.cwd(), 'e2e', 'fixtures', 'hosted_schema_pre_20260807120000.sql');

// The first migration the fixture does NOT already contain.
const FIRST_REPLAYED = '20260807120000';

// What the Supabase platform does that the bare e2e shim does not, and which
// decides every grant this file checks.
//
// Default privileges: supabase/postgres' init scripts run
// `ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES / FUNCTIONS /
// SEQUENCES TO anon, authenticated, service_role`, so every table and function
// the migrations create arrives with explicit client grants, on top of the
// EXECUTE Postgres gives PUBLIC. Default privileges are per database, so this
// stays inside this file's database.
//
// auth.uid() / auth.role(): the shim hardwires role() to 'service_role' because
// its suites connect as a superuser and never meet RLS. Here the client roles do
// meet it, so these are GoTrue's real definitions, reading the JWT claims
// PostgREST sets per request.
const SUPABASE_PLATFORM = `
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO anon, authenticated, service_role;
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON FUNCTIONS TO anon, authenticated, service_role;
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON SEQUENCES TO anon, authenticated, service_role;
GRANT USAGE ON SCHEMA auth TO anon, authenticated, service_role;
CREATE OR REPLACE FUNCTION auth.uid() RETURNS uuid LANGUAGE sql STABLE AS $$
  SELECT coalesce(
    nullif(current_setting('request.jwt.claim.sub', true), ''),
    (nullif(current_setting('request.jwt.claims', true), '')::jsonb ->> 'sub')
  )::uuid
$$;
CREATE OR REPLACE FUNCTION auth.role() RETURNS text LANGUAGE sql STABLE AS $$
  SELECT coalesce(
    nullif(current_setting('request.jwt.claim.role', true), ''),
    (nullif(current_setting('request.jwt.claims', true), '')::jsonb ->> 'role')
  )::text
$$;
`;

// Same query as db_grants.test.ts. Trigger functions are exempt: PostgREST does
// not expose them.
const EXPOSED_SECDEF = `
  SELECT p.oid::regprocedure::text AS fn,
         has_function_privilege('anon', p.oid, 'EXECUTE') AS anon,
         has_function_privilege('authenticated', p.oid, 'EXECUTE') AS authed
  FROM pg_proc p
  JOIN pg_namespace n ON n.oid = p.pronamespace
  WHERE n.nspname = 'public'
    AND p.prosecdef
    AND p.prorettype <> 'trigger'::regtype
    AND (has_function_privilege('anon', p.oid, 'EXECUTE')
      OR has_function_privilege('authenticated', p.oid, 'EXECUTE'))
  ORDER BY 1
`;

// Every non-SELECT privilege a client role holds on a public relation, at table
// level or on any column. PUBLIC grants count, since both roles inherit them.
const CLIENT_WRITES = `
  SELECT c.relname || ' ' || r.role || ' ' || p.priv AS grant
  FROM pg_class c
  JOIN pg_namespace n ON n.oid = c.relnamespace
  CROSS JOIN (VALUES ('anon'), ('authenticated')) AS r(role)
  CROSS JOIN (VALUES ('INSERT'), ('UPDATE'), ('DELETE'), ('TRUNCATE'), ('REFERENCES'), ('TRIGGER')) AS p(priv)
  WHERE n.nspname = 'public'
    AND c.relkind IN ('r', 'p', 'v', 'm', 'f')
    AND CASE WHEN p.priv IN ('INSERT', 'UPDATE', 'REFERENCES')
             THEN has_any_column_privilege(r.role, c.oid, p.priv)
             ELSE has_table_privilege(r.role, c.oid, p.priv) END
  ORDER BY 1
`;

// Client writes that are the product, each with the reason. Everything else a
// client changes goes through an edge function holding the service-role key.
const INTENDED_CLIENT_WRITES: Record<string, string> = {
    'chat_messages authenticated INSERT':
        'the web client sends chat with a direct INSERT (src/contexts/ServerContext.tsx); '
        + 'the "Players can send chat messages to their games" policy limits it to your own '
        + 'user_id in a game you are a member of',
};

// SECURITY DEFINER functions a client may call on purpose. None today: every
// .rpc() caller is an edge function with the service-role key.
const INTENDED_CLIENT_RPCS: Record<string, string> = {};

const WRITE_POLICIES = `
  SELECT tablename || ': ' || policyname || ' (' || cmd || ')' AS policy
  FROM pg_policies
  WHERE schemaname = 'public' AND cmd <> 'SELECT'
  ORDER BY 1
`;

// The games table and the functions that write it, as the database holds them:
// columns with their types, nullability and defaults (of games and of the
// tables its writers fill with blobs), indexes, triggers, the snapshot read
// policy, and
// each writer's definition with comments and whitespace folded away (seed.sql
// explains more than a migration does; the code must be the same).
const GAMES_SHAPE = `
  SELECT 'column ' || table_name || '.' || column_name || ' ' || data_type || ' ' || udt_name || ' ' || is_nullable || ' ' || coalesce(column_default, '') AS item
  FROM information_schema.columns
  WHERE table_schema = 'public' AND table_name IN ('games', 'player_views', 'spectator_views', 'game_snapshots')
  UNION ALL
  SELECT 'index ' || indexdef FROM pg_indexes WHERE schemaname = 'public' AND tablename IN ('games', 'game_snapshots')
  UNION ALL
  SELECT 'policy ' || tablename || ' ' || policyname || ' ' || cmd || ' ' || coalesce(qual, '') || ' ' || coalesce(with_check, '')
  FROM pg_policies WHERE schemaname = 'public' AND tablename = 'game_snapshots'
  UNION ALL
  SELECT 'trigger ' || pg_get_triggerdef(oid) FROM pg_trigger WHERE tgrelid = 'public.games'::regclass AND NOT tgisinternal
  UNION ALL
  SELECT 'function ' || p.oid::regprocedure::text || ' ' || md5(regexp_replace(regexp_replace(
           pg_get_functiondef(p.oid), '--[^\\n]*', '', 'g'), '\\s+', ' ', 'g'))
  FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
  WHERE n.nspname = 'public'
    AND (p.proname IN ('commit_game', 'create_game', 'commit_table', 'create_table', 'delete_account') OR p.proname LIKE 'legacy\\_%')
  ORDER BY 1
`;

// The WHOLE of schema public, as the catalogs hold it, order-insensitive.
//
// GAMES_SHAPE above compares the games family; this compares everything, so the
// migration chain and seed.sql are held equal object by object rather than in
// the four places somebody remembered to list. It is what makes seed.sql
// readable as the schema without the migration history being collapsed into it:
// the two paths are proven to end in the same database on every run.
//
// Physical column ORDER is deliberately not compared. The chain grew games and
// user_elo_ratings a column at a time, seed.sql declares them in the order of
// docs/C_GAME_SHAPE_MIGRATION.md 3.1, and nothing reads a column by position
// (PostgREST answers named JSON, every server query names its columns). That
// divergence already exists between the hosted database and every local one.
const PUBLIC_CATALOG = `
  SELECT 'column ' || table_name || '.' || column_name || ' ' || data_type || ' ' || udt_name
         || ' null=' || is_nullable || ' default=' || coalesce(column_default, '') AS item
  FROM information_schema.columns WHERE table_schema = 'public'
  UNION ALL
  SELECT 'constraint ' || conrelid::regclass::text || ' ' || conname || ' ' || pg_get_constraintdef(oid)
  FROM pg_constraint WHERE connamespace = 'public'::regnamespace
  UNION ALL
  SELECT 'index ' || indexdef FROM pg_indexes WHERE schemaname = 'public'
  UNION ALL
  SELECT 'trigger ' || tgrelid::regclass::text || ' ' || pg_get_triggerdef(oid)
  FROM pg_trigger WHERE NOT tgisinternal AND tgrelid::regclass::text NOT LIKE 'pg\\_%'
  UNION ALL
  SELECT 'policy ' || schemaname || '.' || tablename || ' ' || policyname || ' ' || cmd
         || ' roles=' || array_to_string(roles, ',') || ' permissive=' || permissive
         || ' using=' || coalesce(qual, '') || ' check=' || coalesce(with_check, '')
  FROM pg_policies
  UNION ALL
  SELECT 'rls ' || c.relname || ' enabled=' || c.relrowsecurity || ' forced=' || c.relforcerowsecurity
  FROM pg_class c WHERE c.relnamespace = 'public'::regnamespace AND c.relkind IN ('r', 'p')
  UNION ALL
  SELECT 'function ' || p.oid::regprocedure::text || ' secdef=' || p.prosecdef
         || ' vol=' || p.provolatile::text || ' ret=' || pg_get_function_result(p.oid)
         || ' body=' || md5(regexp_replace(regexp_replace(pg_get_functiondef(p.oid), '--[^\\n]*', '', 'g'), '\\s+', ' ', 'g'))
  FROM pg_proc p WHERE p.pronamespace = 'public'::regnamespace
  UNION ALL
  SELECT 'enum ' || t.typname || ' ' || e.enumsortorder || ' ' || e.enumlabel
  FROM pg_enum e JOIN pg_type t ON t.oid = e.enumtypid WHERE t.typnamespace = 'public'::regnamespace
  UNION ALL
  SELECT 'tablegrant ' || c.relname || ' ' || r.role || ' ' || p.priv || ' ' || has_table_privilege(r.role, c.oid, p.priv)
  FROM pg_class c
  CROSS JOIN (VALUES ('anon'), ('authenticated'), ('service_role'), ('public')) AS r(role)
  CROSS JOIN (VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('DELETE'), ('TRUNCATE'), ('REFERENCES'), ('TRIGGER')) AS p(priv)
  WHERE c.relnamespace = 'public'::regnamespace AND c.relkind IN ('r', 'p', 'v', 'm', 'f')
  UNION ALL
  SELECT 'colgrant ' || c.relname || '.' || a.attname || ' ' || r.role || ' ' || p.priv || ' '
         || has_column_privilege(r.role, c.oid, a.attnum, p.priv)
  FROM pg_class c JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped
  CROSS JOIN (VALUES ('anon'), ('authenticated'), ('service_role'), ('public')) AS r(role)
  CROSS JOIN (VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('REFERENCES')) AS p(priv)
  WHERE c.relnamespace = 'public'::regnamespace AND c.relkind IN ('r', 'p', 'v', 'm', 'f')
  UNION ALL
  SELECT 'fngrant ' || p.oid::regprocedure::text || ' ' || r.role || ' ' || has_function_privilege(r.role, p.oid, 'EXECUTE')
  FROM pg_proc p CROSS JOIN (VALUES ('anon'), ('authenticated'), ('service_role'), ('public')) AS r(role)
  WHERE p.pronamespace = 'public'::regnamespace
  UNION ALL
  SELECT 'seqgrant ' || c.relname || ' ' || r.role || ' ' || p.priv || ' ' || has_sequence_privilege(r.role, c.oid, p.priv)
  FROM pg_class c CROSS JOIN (VALUES ('anon'), ('authenticated'), ('service_role'), ('public')) AS r(role)
  CROSS JOIN (VALUES ('SELECT'), ('UPDATE'), ('USAGE')) AS p(priv)
  WHERE c.relnamespace = 'public'::regnamespace AND c.relkind = 'S'
  UNION ALL
  SELECT 'publication ' || pubname || ' ' || schemaname || '.' || tablename FROM pg_publication_tables
  UNION ALL
  SELECT 'comment ' || c.relname || ' :: ' || obj_description(c.oid, 'pg_class')
  FROM pg_class c WHERE c.relnamespace = 'public'::regnamespace AND c.relkind IN ('r', 'p', 'v', 'm')
    AND obj_description(c.oid, 'pg_class') IS NOT NULL
  UNION ALL
  SELECT 'comment ' || c.relname || '.' || a.attname || ' :: ' || col_description(c.oid, a.attnum)
  FROM pg_class c JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped
  WHERE c.relnamespace = 'public'::regnamespace AND c.relkind IN ('r', 'p', 'v', 'm')
    AND col_description(c.oid, a.attnum) IS NOT NULL
  ORDER BY 1
`;

type Exposure = { fn: string; who: string };
type Posture = { exposed: string[]; writes: string[]; writePolicies: string[] };

async function exposedRpcs(): Promise<Exposure[]> {
    const { rows } = await pgPool.query(EXPOSED_SECDEF);
    return rows
        .filter((r) => !(r.fn.replace(/\(.*$/, '') in INTENDED_CLIENT_RPCS))
        .map((r) => ({ fn: r.fn, who: [r.anon && 'anon', r.authed && 'authenticated'].filter(Boolean).join('+') }));
}

async function posture(): Promise<Posture> {
    const exposed = (await exposedRpcs()).map((e) => `${e.fn} [${e.who}]`);
    const writes = (await pgPool.query(CLIENT_WRITES)).rows.map((r) => r.grant);
    const writePolicies = (await pgPool.query(WRITE_POLICIES)).rows.map((r) => r.policy);
    return { exposed, writes, writePolicies };
}

function replayedMigrations(): string[] {
    return readdirSync(MIGRATIONS)
        .filter((f) => /^\d{14}_.*\.sql$/.test(f) && f.slice(0, 14) >= FIRST_REPLAYED)
        .sort();
}

// Run `sql` as a client role carrying a JWT, the way PostgREST runs a request,
// and roll it back.
async function asClient<T>(role: 'anon' | 'authenticated', sub: string | null, fn: (c: PoolClient) => Promise<T>): Promise<T> {
    const c = await pgPool.connect();
    try {
        await c.query('BEGIN');
        await c.query(`SET LOCAL ROLE ${role}`);
        await c.query(`SELECT set_config('request.jwt.claims', $1, true)`, [JSON.stringify({ role, sub })]);
        return await fn(c);
    } finally {
        await c.query('ROLLBACK').catch(() => {});
        c.release();
    }
}

const ATTACKER = '00000000-0000-4000-8000-00000000a77c';
const MEMBER = '00000000-0000-4000-8000-0000000be4be';
const VICTIM_GAME = 'victim-game';

let hostedPosture: Posture | null = null;
let hostedGamesShape: string[] | null = null;
let hostedCatalog: string[] | null = null;

export function registerMigrationGrantsValidation(): void {
    describe('hosted schema: the frozen pre-20260807120000 schema plus every later migration', () => {
        // fn -> the migration after which it became client-callable, cleared again
        // if a later migration locks it.
        const openedBy = new Map<string, string>();
        let baselineExposed: Exposure[] = [];
        let replayed: string[] = [];

        before(async () => {
            await applyPlatformShim();
            await pgPool.query(SUPABASE_PLATFORM);
            await pgPool.query(readFileSync(FIXTURE, 'utf8'));
            baselineExposed = await exposedRpcs();
            for (const x of baselineExposed) openedBy.set(`${x.fn} [${x.who}]`, 'the frozen hosted schema');

            replayed = replayedMigrations();
            for (const m of replayed) {
                try {
                    await pgPool.query(readFileSync(join(MIGRATIONS, m), 'utf8'));
                } catch (e) {
                    throw new Error(`migration ${m} does not apply on top of the hosted schema: ${(e as Error).message}`);
                }
                const now = new Set((await exposedRpcs()).map((x) => `${x.fn} [${x.who}]`));
                for (const k of now) if (!openedBy.has(k)) openedBy.set(k, m);
                for (const k of [...openedBy.keys()]) if (!now.has(k)) openedBy.delete(k);
            }
            hostedGamesShape = (await pgPool.query(GAMES_SHAPE)).rows.map((r) => r.item);
            hostedCatalog = (await pgPool.query(PUBLIC_CATALOG)).rows.map((r) => r.item);

            await pgPool.query(
                `INSERT INTO games (id, name, players, status) VALUES ($1, 'victim', '[]'::jsonb, 'waiting')`,
                [VICTIM_GAME],
            );
            await pgPool.query(`INSERT INTO auth.users (id) VALUES ($1), ($2)`, [MEMBER, ATTACKER]);
            await pgPool.query(`INSERT INTO player_hands (game_id, player_id) VALUES ($1, $2)`, [VICTIM_GAME, MEMBER]);
            hostedPosture = await posture();
        });

        test('the emulation is not vacuous: the pre-lockdown schema really exposed the state RPCs', () => {
            // If the platform defaults stopped granting, every assertion below would
            // pass on a database that exposes nothing to begin with.
            const names = baselineExposed.map((e) => e.fn.replace(/\(.*$/, ''));
            for (const rpc of ['commit_game', 'create_game', 'try_acquire_bot_lease']) {
                assert.ok(names.includes(rpc), `expected ${rpc} exposed before 20260807120000, got ${names.join(', ')}`);
            }
            assert.ok(replayed.includes('20260906120000_drop_json_hand_columns.sql'), `replayed: ${replayed.join(', ')}`);
        });

        test('no SECURITY DEFINER function is client-callable after the last migration', () => {
            const offenders = [...openedBy].map(([k, m]) => `${k} (opened by ${m})`);
            assert.deepEqual(
                offenders, [],
                'A migration left a SECURITY DEFINER function executable by a client role on the hosted '
                + 'database. DROP + CREATE (unlike CREATE OR REPLACE) gives the function fresh default '
                + 'grants to PUBLIC, anon and authenticated, so every migration that recreates one must '
                + 'revoke them again (see 20260917000000_relock_rpcs_and_games_writes.sql). '
                + `Exposed: ${offenders.join(', ')}`,
            );
        });

        test('anon cannot execute commit_table, as POST /rest/v1/rpc/commit_table would', async () => {
            await asClient('anon', null, async (c) => {
                await assert.rejects(
                    // A game id that matches nothing: if EXECUTE is granted this returns
                    // committed = false and touches no row.
                    c.query(`SELECT * FROM commit_table(p_game_id => 'no-such-game', p_expected_version => 0, p_state => 'AA==', p_status => 0::smallint, p_needs_bots => FALSE)`),
                    (e: { code?: string }) => e.code === '42501',
                    'anon executed commit_table, the RPC that rewrites any game',
                );
            });
        });

        test('anon cannot execute commit_game, as POST /rest/v1/rpc/commit_game would', async () => {
            // The legacy writer is still here until the contract migration drops it,
            // and 20260807120000 plus 20260917000000 must keep it off the client roles.
            await asClient('anon', null, async (c) => {
                await assert.rejects(
                    c.query(`SELECT commit_game('no-such-game', 0, '{}'::jsonb)`),
                    (e: { code?: string }) => e.code === '42501',
                    'anon executed commit_game, the RPC that rewrites any game',
                );
            });
        });

        test('no client role holds a write privilege on a public table, beyond the intended ones', () => {
            const unexpected = hostedPosture!.writes.filter((w) => !(w in INTENDED_CLIENT_WRITES));
            assert.deepEqual(
                unexpected, [],
                'Supabase grants ALL on every new public table to anon and authenticated, so RLS is '
                + 'the only thing standing between a client and the table. Revoke the writes (see '
                + '20260917000000_relock_rpcs_and_games_writes.sql) or add the grant to '
                + `INTENDED_CLIENT_WRITES with its reason. Unexpected: ${unexpected.join(', ')}`,
            );
        });

        test('an authenticated user cannot create a game row directly', async () => {
            // The edge function creates games through create_game with the service
            // role. A row a user inserts themselves is a game whose players, seats
            // and state blob they chose, which the action endpoint then plays and
            // scores into the listed players' ELO.
            await asClient('authenticated', ATTACKER, async (c) => {
                await assert.rejects(
                    c.query(
                        `INSERT INTO games (id, status, state, roster)
                         VALUES ('forged', 'playing', '\\xdeadbeef', $1)`,
                        [`\\x01${Buffer.from(ATTACKER).toString('hex')}`],
                    ),
                    (e: { code?: string }) => e.code === '42501',
                    'authenticated inserted a games row directly',
                );
            });
            await asClient('anon', null, async (c) => {
                await assert.rejects(
                    c.query(`INSERT INTO games (id, name) VALUES ('forged-anon', 'forged')`),
                    (e: { code?: string }) => e.code === '42501',
                );
            });
        });

        test('neither client role can update or delete an existing game', async () => {
            for (const role of ['anon', 'authenticated'] as const) {
                await asClient(role, role === 'anon' ? null : ATTACKER, async (c) => {
                    for (const sql of [
                        `UPDATE games SET status = 'game_over' WHERE id = $1`,
                        `DELETE FROM games WHERE id = $1`,
                    ]) {
                        // Denied outright or filtered to zero rows by RLS; either way the
                        // row must be untouched.
                        await c.query('SAVEPOINT s');
                        const affected = await c.query(sql, [VICTIM_GAME]).then((r) => r.rowCount, () => 0);
                        await c.query('ROLLBACK TO SAVEPOINT s');
                        assert.equal(affected, 0, `${role}: ${sql}`);
                    }
                });
            }
            const { rows } = await pgPool.query('SELECT status FROM games WHERE id = $1', [VICTIM_GAME]);
            assert.deepEqual(rows, [{ status: 'waiting' }]);
        });

        test('the intended client write still works: a member sends chat, a non-member cannot', async () => {
            // The revoke must not take the product with it. Chat is the one write a
            // client makes directly.
            const send = (c: PoolClient, user: string) => c.query(
                `INSERT INTO chat_messages (game_id, user_id, message) VALUES ($1, $2, 'hi')`, [VICTIM_GAME, user]);
            await asClient('authenticated', MEMBER, async (c) => {
                assert.equal((await send(c, MEMBER)).rowCount, 1);
            });
            await asClient('authenticated', ATTACKER, async (c) => {
                await assert.rejects(send(c, ATTACKER), (e: { code?: string }) => e.code === '42501');
            });
        });

        test('service_role keeps EXECUTE on every state RPC', async () => {
            const { rows } = await pgPool.query(
                `SELECT p.oid::regprocedure::text AS fn
                 FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
                 WHERE n.nspname = 'public' AND p.prosecdef AND p.prorettype <> 'trigger'::regtype
                   AND NOT has_function_privilege('service_role', p.oid, 'EXECUTE')`,
            );
            assert.deepEqual(rows.map((r) => r.fn), []);
        });
    });

    describe('fresh schema: seed.sql under the same platform defaults', () => {
        before(async () => {
            // Reset in place rather than recreating the database: the shim drops and
            // recreates public, auth and realtime. DROP DATABASE ... WITH (FORCE)
            // would kill the pool's idle connections under it.
            await pgPool.query(readFileSync(join(process.cwd(), 'e2e', 'schema.sql'), 'utf8'));
            await pgPool.query(SUPABASE_PLATFORM);
            await pgPool.query(readFileSync(join(SUPABASE, 'seed.sql'), 'utf8'));
        });

        test('seed.sql exposes no SECURITY DEFINER function and no unintended table write', async () => {
            const p = await posture();
            assert.deepEqual(p.exposed, [], `exposed: ${p.exposed.join(', ')}`);
            const unexpected = p.writes.filter((w) => !(w in INTENDED_CLIENT_WRITES));
            assert.deepEqual(unexpected, [], `unexpected client writes: ${unexpected.join(', ')}`);
        });

        test('seed.sql and the migrations build the same games table and games writers', async () => {
            // A fresh database (seed.sql) and the hosted one (the migrations) run the
            // same edge functions, so a column, trigger or writer that differs
            // between them is a server that passes locally and breaks on deploy.
            assert.ok(hostedGamesShape, 'the hosted suite must run first');
            const seedShape = (await pgPool.query(GAMES_SHAPE)).rows.map((r) => r.item);
            assert.ok(seedShape.some((i) => i.startsWith('function commit_table(')), 'the comparison sees the table writers');
            assert.deepEqual(seedShape, hostedGamesShape);
        });

        test('seed.sql and the migrations build the same schema public, object for object', async () => {
            // The whole of it: columns, constraints, indexes, triggers, policies, RLS,
            // functions with their bodies, enum values, every table, column, function
            // and sequence grant, the realtime publication, and the comments.
            //
            // This is the equivalence the migration history would have to hold for it
            // to be safe to collapse into seed.sql, checked on every run instead of
            // once by hand (docs/C_GAME_SHAPE_MIGRATION.md, "Cleanup as built").
            assert.ok(hostedCatalog, 'the hosted suite must run first');
            const seedCatalog: string[] = (await pgPool.query(PUBLIC_CATALOG)).rows.map((r) => r.item);
            assert.ok(seedCatalog.length > 1000, `the comparison is not vacuous: ${seedCatalog.length} items`);
            // The symmetric difference, not the two 1,400-item lists: a deepEqual of
            // those prints neither the object that differs nor how.
            const hosted = new Set(hostedCatalog);
            const seed = new Set(seedCatalog);
            const differences = [
                ...seedCatalog.filter((i) => !hosted.has(i)).map((i) => `seed.sql only: ${i}`),
                ...hostedCatalog!.filter((i) => !seed.has(i)).map((i) => `migrations only: ${i}`),
            ].sort();
            assert.deepEqual(
                differences, [],
                'seed.sql and the migrations end in different databases. The hosted project only ever '
                + 'receives the migrations and every other database is built from seed.sql, so anything '
                + 'that differs here passes locally and breaks on deploy (or the other way round).',
            );
        });

        test('seed.sql and the migrations end in the same security posture', async () => {
            assert.ok(hostedPosture, 'the hosted suite must run first');
            assert.deepEqual(
                await posture(), hostedPosture,
                'seed.sql and the migrations disagree on client grants or write policies. The hosted '
                + 'project only ever sees the migrations; every security change goes in both.',
            );
        });
    });
}

if (!process.env.VALIDATION_ONLY) {
    registerMigrationGrantsValidation();
    after(async () => { await pgPool.end(); });
}
