/* =============================================================================
 * commit_table rates a finished game, in the commit that ends it
 * =============================================================================
 * A game's rating changes used to be written by the edge function AFTER the
 * commit that ended the game: read each seat's rating, add the kernel's change,
 * write the sum back. Two games that shared a seat and ended together both read
 * the same rating, and the second write erased the first game - and every bot
 * sits at many tables. commit_table applies the changes itself now
 * (server/impls/supabase/seed.sql), and this file holds what that buys:
 *
 *   - a change is ADDED to the row as it stands, so two games that share a seat
 *     both count, whichever read the rating first;
 *   - it happens behind the version fence, so the commit that loses the race
 *     changes no rating, and a game is rated once;
 *   - a rating does not go below zero, a human with no row starts from 1000, and
 *     a seat whose account is gone is skipped without failing the game's end.
 *
 * The migration that takes hosted there is held by
 * e2e/commit_table_ratings_migration.test.ts.
 *
 * Real Postgres, no kernel: the state a commit stores is opaque to the database.
 * ========================================================================== */

import './harness.ts';
import { describe, test, before } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, pgPool, uuid } from './harness.ts';

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

describe('commit_table rates the game it ends', () => {
    before(async () => { await applySchema(); });

    test('each seat\'s change is added to its row: humans in user_elo_ratings, bots in bots', async () => {
        const [g, a, b, m] = [await game(), await human(), await human(), await bot(1210)];
        assert.equal(await endGame(g, 0, [{ id: a, bot: false, delta: 7 }, { id: b, bot: false, delta: -3 }, { id: m, bot: true, delta: -4 }]), true);
        assert.deepEqual(await ratingOf(a), { elo: 1007, previous: 1000, played: 1 });
        assert.deepEqual(await ratingOf(b), { elo: 997, previous: 1000, played: 1 });
        assert.deepEqual(await ratingOf(m), { elo: 1206, previous: 1210, played: 1 });
    });

    test('a commit that rates nobody changes no rating (every commit but a rated game\'s last)', async () => {
        const [g, a] = [await game(), await human()];
        const { rows } = await pgPool.query(
            `SELECT committed FROM commit_table(p_game_id => $1, p_expected_version => 0, p_state => 'AA==', p_status => 2::smallint, p_needs_bots => FALSE)`, [g]);
        assert.equal(rows[0].committed, true);
        assert.deepEqual(await ratingOf(a), { elo: 1000, previous: 1000, played: 0 });
    });

    test('two games that share a seat BOTH count, though each worked its change out from the same rating', async () => {
        // The lost update: both games read the bot at 1000. The old writer stored
        // 1000 + its own change, so the second game to finish erased the first.
        const [g1, g2, m, a, b] = [await game(), await game(), await bot(1000), await human(), await human()];
        assert.equal(await endGame(g1, 0, [{ id: m, bot: true, delta: 5 }, { id: a, bot: false, delta: -5 }]), true);
        assert.equal(await endGame(g2, 0, [{ id: m, bot: true, delta: -6 }, { id: b, bot: false, delta: 6 }]), true);
        assert.deepEqual(await ratingOf(m), { elo: 999, previous: 1005, played: 2 }, 'the bot carries both games: 1000 + 5 - 6');
    });

    test('the commit that loses the version fence rates nobody: a game is rated once', async () => {
        const [g, a, b] = [await game(), await human(), await human()];
        const seats = [{ id: a, bot: false, delta: 5 }, { id: b, bot: false, delta: -5 }];
        assert.equal(await endGame(g, 0, seats), true, 'the first writer ends the game');
        assert.equal(await endGame(g, 0, seats), false, 'a second writer at the same version is refused');
        assert.deepEqual(await ratingOf(a), { elo: 1005, previous: 1000, played: 1 });
        assert.deepEqual(await ratingOf(b), { elo: 995, previous: 1000, played: 1 });
    });

    test('a rating stops at zero, a human with no row starts from 1000, and a seat with no account is skipped', async () => {
        const [g, low, fresh, gone] = [await game(), await bot(3), await human(), uuid()];
        await pgPool.query('DELETE FROM user_elo_ratings WHERE user_id = $1', [fresh]);
        assert.equal(await endGame(g, 0, [
            { id: low, bot: true, delta: -10 }, { id: fresh, bot: false, delta: 6 }, { id: gone, bot: false, delta: 4 },
        ]), true, 'the game still ends');
        assert.deepEqual(await ratingOf(low), { elo: 0, previous: 3, played: 1 });
        assert.deepEqual(await ratingOf(fresh), { elo: 1006, previous: 1000, played: 1 });
        assert.equal(await ratingOf(gone), null, 'no row is invented for an account that does not exist');
    });

    test('the three rating arguments go together', async () => {
        const [g, a] = [await game(), await human()];
        await assert.rejects(pgPool.query(
            `SELECT * FROM commit_table(p_game_id => $1, p_expected_version => 0, p_state => 'AA==', p_status => 2::smallint, p_needs_bots => FALSE,
                p_rated_ids => $2::uuid[], p_rated_bots => '{f}'::boolean[], p_rated_deltas => '{1,2}'::integer[])`, [g, [a]]),
            /go together/);
        await assert.rejects(pgPool.query(
            `SELECT * FROM commit_table(p_game_id => $1, p_expected_version => 0, p_state => 'AA==', p_status => 2::smallint, p_needs_bots => FALSE,
                p_rated_ids => $2::uuid[])`, [g, [a]]),
            /go together/);
        assert.deepEqual(await ratingOf(a), { elo: 1000, previous: 1000, played: 0 });
    });
});
