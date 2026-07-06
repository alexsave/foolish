// E2E: leaderboard data path — the REAL create_default_elo_rating trigger
// (seed.sql / migration 20260702090000) stamping usernames onto the
// world-readable user_elo_ratings rows, and the exact standings query the
// /leaderboard screen runs.

import './harness.ts';
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';

const newUser = async (username: string): Promise<string> => {
    const id = uuid();
    await pgPool.query(
        'INSERT INTO auth.users(id, raw_user_meta_data) VALUES($1, $2)',
        [id, JSON.stringify({ username })],
    );
    return id;
};

before(async () => { await applySchema(); await resetDb(); });
after(async () => { await pgPool.end(); });

test('leaderboard: signup trigger stamps the username onto the rating row', async () => {
    const id = await newUser('ALICE');
    const { rows } = await pgPool.query(
        'SELECT username, elo_rating, games_played FROM user_elo_ratings WHERE user_id = $1',
        [id],
    );
    assert.equal(rows.length, 1, 'rating row auto-created');
    assert.equal(rows[0].username, 'ALICE');
    assert.equal(rows[0].elo_rating, 1000);
    assert.equal(rows[0].games_played, 0);
});

test('leaderboard: a metadata rename propagates to the denormalized username', async () => {
    const id = await newUser('BEFORE');
    await pgPool.query(
        `UPDATE auth.users SET raw_user_meta_data = jsonb_set(raw_user_meta_data, '{username}', '"AFTER"') WHERE id = $1`,
        [id],
    );
    const { rows } = await pgPool.query(
        'SELECT username FROM user_elo_ratings WHERE user_id = $1', [id],
    );
    assert.equal(rows[0].username, 'AFTER', 'rename reached the rating row');
});

test('leaderboard: standings query ranks players by elo and hides the unrated', async () => {
    const strong = await newUser('STRONG');
    const weak = await newUser('WEAK');
    await newUser('NEVER_PLAYED'); // stays at 0 games — must not appear

    await pgPool.query(
        'UPDATE user_elo_ratings SET elo_rating = $2, games_played = 12 WHERE user_id = $1',
        [strong, 1180],
    );
    await pgPool.query(
        'UPDATE user_elo_ratings SET elo_rating = $2, games_played = 7 WHERE user_id = $1',
        [weak, 940],
    );

    // Same shape as Leaderboard.tsx: rated players only, best first.
    const { rows } = await pgPool.query(
        `SELECT username, elo_rating FROM user_elo_ratings
         WHERE games_played > 0 ORDER BY elo_rating DESC LIMIT 100`,
    );
    assert.deepEqual(
        rows.map((r) => r.username),
        ['STRONG', 'WEAK'],
        'rated players in elo order, unrated hidden',
    );
});
