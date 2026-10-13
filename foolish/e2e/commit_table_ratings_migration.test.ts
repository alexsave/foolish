/* =============================================================================
 * The migration that teaches hosted's commit_table to rate
 * =============================================================================
 * migrations/20261013120000_commit_table_ratings.sql, run against a real
 * Postgres shaped like the hosted project: the schema, commit_table as it is
 * deployed today (fifteen arguments, e2e/fixtures/commit_table_pre_ratings.sql),
 * and Supabase's default privileges, which hand every NEW function to anon and
 * authenticated.
 *
 * Three ways this migration could take production down, each held here:
 *   - CREATE OR REPLACE with a longer argument list does not replace: it adds a
 *     second function, and every call that names only the old arguments becomes
 *     ambiguous. So: exactly ONE commit_table afterwards.
 *   - The edge functions deployed at that moment still name fifteen arguments.
 *     So: such a call commits.
 *   - A function that is dropped and created again comes back executable by
 *     anon. So: no client role can execute it.
 *
 * What the function then DOES with a rating is e2e/commit_table_ratings.test.ts.
 * This file goes when the migration does (migrations/README.md: a delta is
 * deleted once hosted has it).
 * ========================================================================== */

import './harness.ts';
import { describe, test, before } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { applyPlatformShim, pgPool, uuid } from './harness.ts';

const SUPABASE = join(process.cwd(), 'server', 'impls', 'supabase');
const MIGRATION = readFileSync(join(SUPABASE, 'migrations', '20261013120000_commit_table_ratings.sql'), 'utf8');

/** A games row at version 0 that a commit can be fenced on. */
async function game(): Promise<string> {
    const id = `r${uuid().slice(0, 7)}`;
    await pgPool.query(`INSERT INTO games (id, status, state, roster, needs_bots, version) VALUES ($1, 'playing', '\\x00', '\\x00', FALSE, 0)`, [id]);
    return id;
}
/** A signed-up human: the sign-up trigger gives them their 1000 row. */
async function human(): Promise<string> {
    const id = uuid();
    await pgPool.query('INSERT INTO auth.users(id) VALUES ($1)', [id]);
    return id;
}
async function bot(rating = 1000): Promise<string> {
    const { rows } = await pgPool.query(`INSERT INTO bots (nickname, strategy_key, elo_rating) VALUES ('%Miami 1', 'random', $1) RETURNING id`, [rating]);
    return rows[0].id;
}
interface Rating { elo: number; previous: number; played: number }
async function ratingOf(id: string): Promise<Rating | null> {
    const { rows } = await pgPool.query(
        `SELECT elo_rating, previous_elo, games_played FROM user_elo_ratings WHERE user_id = $1
         UNION ALL SELECT elo_rating, previous_elo, games_played FROM bots WHERE id = $1`, [id]);
    return rows[0] ? { elo: rows[0].elo_rating, previous: rows[0].previous_elo, played: rows[0].games_played } : null;
}
/** The commit that ends `gameId`, rating `seats`. Returns whether it won the fence. */
async function endGame(gameId: string, expected: number, seats: { id: string; bot: boolean; delta: number }[]): Promise<boolean> {
    const { rows } = await pgPool.query(
        `SELECT committed FROM commit_table(p_game_id => $1, p_expected_version => $2, p_state => 'AA==', p_status => 2::smallint, p_needs_bots => FALSE,
            p_rated_ids => $3::uuid[], p_rated_bots => $4::boolean[], p_rated_deltas => $5::integer[])`,
        [gameId, expected, seats.map((s) => s.id), seats.map((s) => s.bot), seats.map((s) => s.delta)]);
    return rows[0].committed;
}

describe('the migration, on a database shaped like hosted', () => {
    const FIFTEEN = `SELECT committed FROM commit_table(p_game_id => $1, p_expected_version => 0, p_state => 'AA==', p_status => 1::smallint, p_needs_bots => FALSE,
        p_roster => NULL, p_seats => NULL, p_bot_seats => NULL, p_logs_packed => NULL, p_logs_reset => FALSE, p_game_seed => NULL,
        p_view_players => NULL, p_views => NULL, p_spectator => NULL, p_closed_round => FALSE)`;
    const functions = async () => (await pgPool.query(
        `SELECT p.oid::regprocedure::text AS fn, p.pronargs AS n,
                has_function_privilege('anon', p.oid, 'EXECUTE') AS anon,
                has_function_privilege('authenticated', p.oid, 'EXECUTE') AS authenticated,
                has_function_privilege('service_role', p.oid, 'EXECUTE') AS service
         FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE n.nspname = 'public' AND p.proname = 'commit_table'`)).rows;

    before(async () => { await applyPlatformShim(); });

    test('on a database with no schema it declines to run', async () => {
        // `supabase start` runs migrations BEFORE seed.sql (migrations/README.md).
        await pgPool.query(MIGRATION);
        assert.deepEqual(await functions(), [], 'it created nothing: seed.sql is what builds this database');
    });

    test('it replaces the fifteen-argument function with one that the deployed callers can still call, locked to the service role', async () => {
        // Hosted today: the schema, with commit_table as main has it, under the
        // default privileges Supabase gives every new function.
        await pgPool.query('ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON FUNCTIONS TO anon, authenticated, service_role');
        await pgPool.query(readFileSync(join(SUPABASE, 'seed.sql'), 'utf8'));
        await pgPool.query('DROP FUNCTION commit_table');
        await pgPool.query(readFileSync(join(process.cwd(), 'e2e', 'fixtures', 'commit_table_pre_ratings.sql'), 'utf8'));
        await pgPool.query('REVOKE ALL ON FUNCTION commit_table FROM PUBLIC, anon, authenticated; GRANT EXECUTE ON FUNCTION commit_table TO service_role');
        const beforeFns = await functions();
        assert.deepEqual(beforeFns.map((f) => [f.n, f.anon, f.service]), [[15, false, true]], 'the starting point is the function main deploys');

        await pgPool.query(MIGRATION);

        const fns = await functions();
        assert.equal(fns.length, 1, `exactly one commit_table: two would make every call ambiguous (${fns.map((f) => f.fn).join(' | ')})`);
        assert.equal(fns[0].n, 18, 'the one that takes the three rating arguments');
        assert.deepEqual([fns[0].anon, fns[0].authenticated, fns[0].service], [false, false, true],
            'a function that is dropped and created again comes back with the platform\'s default grants; the migration takes them away');

        // The edge functions deployed today name fifteen arguments. They must run as before.
        const g = await game();
        assert.equal((await pgPool.query(FIFTEEN, [g])).rows[0].committed, true, 'a fifteen-argument call commits');
        // And the function it left is the rating one.
        const [g2, m] = [await game(), await bot(1000)];
        assert.equal(await endGame(g2, 0, [{ id: m, bot: true, delta: 9 }]), true);
        assert.deepEqual(await ratingOf(m), { elo: 1009, previous: 1000, played: 1 });

        // Run twice (a re-run deploy): still one function, still locked.
        await pgPool.query(MIGRATION);
        const again = await functions();
        assert.deepEqual(again.map((f) => [f.n, f.anon, f.authenticated, f.service]), [[18, false, false, true]]);
    });
});
