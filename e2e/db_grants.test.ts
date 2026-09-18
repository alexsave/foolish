// E2E: no remotely-callable SECURITY DEFINER function is reachable by a client role.
//
// SECURITY DEFINER functions run as their owner and bypass RLS. Postgres grants
// EXECUTE to PUBLIC on every new function, and PostgREST exposes public-schema
// functions as `POST /rest/v1/rpc/<name>`, which anon (the key shipped in the web
// bundle) and authenticated can call. So a SECURITY DEFINER function is a full
// authorization bypass the moment nobody remembers to revoke it. That is exactly
// how commit_game / create_game / the three bot-lease RPCs stayed open between
// July's delete_account lockdown and 20260807120000.
//
// This asserts the invariant instead of a list of names, so the NEXT such
// function fails here rather than shipping open. Trigger functions are exempt:
// PostgREST does not expose them, so they are not remotely callable.
//
// Runs against seed.sql applied verbatim to a real Postgres, so it covers the
// fresh-database path (seed.sql) only. The hosted project never runs seed.sql,
// and this stayed green while 20260906120000 left commit_game open there:
// e2e/db_migration_grants.test.ts replays the migrations the hosted project
// actually receives, under Supabase's default privileges, and also holds the
// table-write invariant.
//
// Owns the grant validation scenarios; the fast runner
// (e2e/validation/db_validation.test.ts) imports `registerDbGrantsValidation`
// and provides the shared DB before/after.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, pgPool } from './harness.ts';

// A function is exposed if either client role holds EXECUTE, whether from the
// default PUBLIC grant or an explicit `GRANT ... TO anon` (the hosted project had
// both, so checking only PUBLIC would have looked clean while standing wide open).
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

const STATE_RPCS = [
    'commit_table', 'create_table', 'delete_account',
    'try_acquire_bot_lease', 'release_bot_lease', 'renew_bot_lease',
];

// Every privilege a client role holds on games, at table level or on any column
// (docs/C_GAME_SHAPE_MIGRATION.md 3.3: no client reads games; the web and iOS
// read player_views and spectator_views).
const GAMES_CLIENT_PRIVILEGES = `
  SELECT r.role || ' ' || p.priv AS grant
  FROM (VALUES ('anon'), ('authenticated')) AS r(role)
  CROSS JOIN (VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('DELETE'), ('TRUNCATE'), ('REFERENCES'), ('TRIGGER')) AS p(priv)
  WHERE CASE WHEN p.priv IN ('SELECT', 'INSERT', 'UPDATE', 'REFERENCES')
             THEN has_any_column_privilege(r.role, 'public.games', p.priv)
             ELSE has_table_privilege(r.role, 'public.games', p.priv) END
  ORDER BY 1
`;

export function registerDbGrantsValidation(): void {
    test('grants: no client-callable SECURITY DEFINER function is reachable by anon/authenticated', async () => {
        const { rows } = await pgPool.query(EXPOSED_SECDEF);
        const offenders = rows.map((r) => {
            const who = [r.anon && 'anon', r.authed && 'authenticated'].filter(Boolean).join('+');
            return `${r.fn} [${who}]`;
        });
        assert.deepEqual(
            offenders, [],
            'SECURITY DEFINER functions bypass RLS and PostgREST exposes them as RPCs. '
            + 'Revoke from PUBLIC, anon and authenticated, then grant to service_role only '
            + '(see server/impls/supabase/migrations/20260807120000_lock_down_state_rpcs.sql). '
            + `Exposed: ${offenders.join(', ')}`,
        );
    });

    test('grants: the state-mutating RPCs are executable by service_role', async () => {
        // The lockdown must not break the edge functions, which call these with
        // the service-role key. REVOKE FROM PUBLIC also strips service_role's
        // implicit grant, so this catches a revoke that forgot the re-grant.
        const { rows } = await pgPool.query(
            `SELECT p.proname,
                    has_function_privilege('service_role', p.oid, 'EXECUTE') AS service
             FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
             WHERE n.nspname = 'public' AND p.proname = ANY($1)`,
            [STATE_RPCS],
        );
        assert.ok(rows.length >= STATE_RPCS.length, `expected all of ${STATE_RPCS.join(', ')}, got ${rows.length}`);
        const broken = rows.filter((r) => !r.service).map((r) => r.proname);
        assert.deepEqual(broken, [], `service_role lost EXECUTE on: ${broken.join(', ')}`);
    });

    test('grants: anon and authenticated hold no privilege at all on games, and games has no policy', async () => {
        const { rows } = await pgPool.query(GAMES_CLIENT_PRIVILEGES);
        assert.deepEqual(rows.map((r) => r.grant), [], 'games holds the unmasked state blob and the roster; no client reads it');
        const policies = await pgPool.query(`SELECT policyname FROM pg_policies WHERE schemaname = 'public' AND tablename = 'games'`);
        assert.deepEqual(policies.rows, []);
    });

    test('grants: the legacy JSONB writers are gone - no commit_game, create_game, legacy_* function or bridge trigger', async () => {
        const { rows } = await pgPool.query(
            `SELECT p.oid::regprocedure::text AS fn FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
             WHERE n.nspname = 'public' AND (p.proname IN ('commit_game', 'create_game') OR p.proname LIKE 'legacy\\_%')`);
        assert.deepEqual(rows.map((r) => r.fn), []);
        const triggers = await pgPool.query(`SELECT tgname FROM pg_trigger WHERE tgrelid = 'public.games'::regclass AND NOT tgisinternal ORDER BY 1`);
        assert.deepEqual(triggers.rows.map((r) => r.tgname), ['update_games_updated_at']);
    });
}

if (!process.env.VALIDATION_ONLY) {
    before(async () => { await applySchema(); });
    beforeEach(async () => { await resetDb(); });
    registerDbGrantsValidation();
    after(async () => { await pgPool.end(); });
}
