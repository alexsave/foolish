// E2E: the schema, built the way a server builds it, exposes no SECURITY DEFINER
// RPC and no table write to a client role.
//
// e2e/db_grants.test.ts asserts the same function invariant over seed.sql as the
// bare harness loads it. This file is the one that matters, because it loads the
// same seed.sql UNDER SUPABASE'S DEFAULT PRIVILEGES - the platform's init scripts
// run `ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES / FUNCTIONS /
// SEQUENCES TO anon, authenticated, service_role`, so on a real project every
// table and function the schema creates ARRIVES client-callable and stays that
// way unless something takes it back. A REVOKE that goes missing is invisible
// without those defaults in place, and a database that never had them cannot
// fail this file's assertions.
//
// That is not hypothetical. It shipped: migration 20260906120000 DROPped
// commit_game and CREATEd it again with a new signature, and DROP + CREATE
// (unlike CREATE OR REPLACE) hands the new function fresh grants to PUBLIC, anon
// and authenticated. For weeks anyone holding the public anon key could call
// commit_game through PostgREST and rewrite any game, while db_grants.test.ts
// stayed green. This file was written to catch that, and it did - by replaying
// the migration history over a frozen copy of the hosted schema.
//
// The migration history is gone (seed.sql is the whole schema now, and it carries
// the lockdown after its own CREATEs), so the replay is gone with it. The
// security posture it existed to assert is not: every scenario below runs against
// the schema a fresh database really gets, as a client role really reaches it,
// through a JWT PostgREST really sets. `the platform defaults are really in
// effect` is what keeps the rest of the file from passing on a database that
// grants nothing to begin with.
//
// Owns the scenarios; the fast runner (e2e/validation/platform_grants_validation.test.ts)
// imports `registerPlatformGrantsValidation`.

import './harness.ts';
import { describe, test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import type { PoolClient } from 'pg';
import { applyPlatformShim, pgPool } from './harness.ts';

const SUPABASE = join(process.cwd(), 'server', 'impls', 'supabase');

// What the Supabase platform does that the bare e2e shim does not, and which
// decides every grant this file checks.
//
// Default privileges: supabase/postgres' init scripts run
// `ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES / FUNCTIONS /
// SEQUENCES TO anon, authenticated, service_role`, so every table and function
// the schema creates arrives with explicit client grants, on top of the
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

// Every privilege a client role holds on games, SELECT included, at table level
// or on any column (docs/C_GAME_SHAPE_MIGRATION.md 3.3).
const GAMES_CLIENT_PRIVILEGES = `
  SELECT r.role || ' ' || p.priv AS grant
  FROM (VALUES ('anon'), ('authenticated')) AS r(role)
  CROSS JOIN (VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('DELETE'), ('TRUNCATE'), ('REFERENCES'), ('TRIGGER')) AS p(priv)
  WHERE CASE WHEN p.priv IN ('SELECT', 'INSERT', 'UPDATE', 'REFERENCES')
             THEN has_any_column_privilege(r.role, 'public.games', p.priv)
             ELSE has_table_privilege(r.role, 'public.games', p.priv) END
  ORDER BY 1
`;

type Exposure = { fn: string; who: string };
type Posture = { exposed: string[]; writes: string[]; writePolicies: string[]; gamesPrivileges: string[] };

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
    const gamesPrivileges = (await pgPool.query(GAMES_CLIENT_PRIVILEGES)).rows.map((r) => r.grant);
    return { exposed, writes, writePolicies, gamesPrivileges };
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

export function registerPlatformGrantsValidation(): void {
    describe('seed.sql under Supabase default privileges', () => {
        before(async () => {
            await applyPlatformShim();
            await pgPool.query(SUPABASE_PLATFORM);
            await pgPool.query(readFileSync(join(SUPABASE, 'seed.sql'), 'utf8'));

            await pgPool.query(
                `INSERT INTO games (id, status, state, roster) VALUES ($1, 'waiting', '\\x00', '\\x00')`,
                [VICTIM_GAME],
            );
            await pgPool.query(`INSERT INTO auth.users (id) VALUES ($1), ($2)`, [MEMBER, ATTACKER]);
            await pgPool.query(`INSERT INTO player_hands (game_id, player_id) VALUES ($1, $2)`, [VICTIM_GAME, MEMBER]);
        });

        test('the platform defaults are really in effect: a new table and function arrive client-callable', async () => {
            // Without this the whole file is vacuous. Every assertion below says
            // "the schema took a grant back"; on a database that never handed one
            // out they would all pass while the real project was wide open. So
            // create a table and a definer function the way the schema creates
            // its own, under the same ALTER DEFAULT PRIVILEGES, and require that
            // anon can reach both before anything revokes them.
            await pgPool.query(`
                CREATE TABLE public.default_privilege_probe (id int);
                CREATE FUNCTION public.default_privilege_probe_fn() RETURNS int
                  LANGUAGE sql SECURITY DEFINER AS $$ SELECT 1 $$;
            `);
            try {
                const { rows } = await pgPool.query(`
                    SELECT has_table_privilege('anon', 'public.default_privilege_probe', 'INSERT') AS ins,
                           has_table_privilege('authenticated', 'public.default_privilege_probe', 'UPDATE') AS upd,
                           has_function_privilege('anon', 'public.default_privilege_probe_fn()', 'EXECUTE') AS exec
                `);
                assert.deepEqual(rows[0], { ins: true, upd: true, exec: true },
                    'ALTER DEFAULT PRIVILEGES is not granting to the client roles, so this file cannot '
                    + 'see a missing REVOKE and every assertion in it is worthless. Fix SUPABASE_PLATFORM.');
            } finally {
                await pgPool.query(`DROP TABLE public.default_privilege_probe; DROP FUNCTION public.default_privilege_probe_fn()`);
            }
        });

        test('no SECURITY DEFINER function is client-callable', async () => {
            const offenders = (await exposedRpcs()).map((e) => `${e.fn} [${e.who}]`);
            assert.deepEqual(
                offenders, [],
                'A SECURITY DEFINER function is executable by a client role on a database built the way a '
                + 'server builds one. Supabase grants EXECUTE on every new function to PUBLIC, anon and '
                + 'authenticated, so every CREATE of a definer function must revoke them again - and a '
                + 'DROP + CREATE has to do it a second time, since (unlike CREATE OR REPLACE) it does not '
                + `keep the old grants. Exposed: ${offenders.join(', ')}`,
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

        test('no legacy writer exists: commit_game, create_game and the bridge are gone', async () => {
            // The kernel writers are commit_table and create_table. A function that
            // does not exist cannot be reopened by a later grant, so the cheapest
            // way to keep the old JSONB writers locked is for them to stay deleted.
            const { rows } = await pgPool.query(
                `SELECT p.oid::regprocedure::text AS fn FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
                 WHERE n.nspname = 'public' AND (p.proname IN ('commit_game', 'create_game') OR p.proname LIKE 'legacy\\_%')`);
            assert.deepEqual(rows.map((r) => r.fn), []);
        });

        test('neither client role holds any privilege on games, SELECT included, under the platform defaults', async () => {
            // Supabase's default privileges grant ALL on every table; seed.sql takes
            // every one back. No client reads games (3.3).
            assert.deepEqual((await posture()).gamesPrivileges, []);
        });

        test('no client role holds a write privilege on a public table, beyond the intended ones', async () => {
            const unexpected = (await posture()).writes.filter((w) => !(w in INTENDED_CLIENT_WRITES));
            assert.deepEqual(
                unexpected, [],
                'Supabase grants ALL on every new public table to anon and authenticated, so RLS is '
                + 'the only thing standing between a client and the table. Revoke the writes in seed.sql '
                + 'or add the grant to INTENDED_CLIENT_WRITES with its reason. '
                + `Unexpected: ${unexpected.join(', ')}`,
            );
        });

        test('an authenticated user cannot create a game row directly', async () => {
            // The edge function creates games through create_table with the service
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
                    c.query(`INSERT INTO games (id, state, roster) VALUES ('forged-anon', '\\x00', '\\x00')`),
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
}

if (!process.env.VALIDATION_ONLY) {
    registerPlatformGrantsValidation();
    after(async () => { await pgPool.end(); });
}
