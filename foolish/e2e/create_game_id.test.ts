// create never hands out a game it did not store.
//
// The game id is six characters of a random UUID (24 bits), so a new id can be
// one an existing game already holds. create used to answer first and store the
// row afterwards, treating a unique violation as "an earlier attempt already
// landed": on a collision the creator got the envelope of a lobby that was never
// written, whose id named somebody else's game. Every later request for it went
// to that other game.
//
// The platform's id draw (crypto.randomUUID) is pinned here to a scripted list,
// through the real create edge function, the real create_table plpgsql and a
// real row already holding the first id.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { mustReadTable } from './helpers/table_play.ts';
import { seedLobby } from './helpers/table_server.ts';
import { postJson, settle, tokenFor } from './helpers/edge.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const TAKEN = 'c0111d';
const FREE = 'c0222d';

/** Runs `body` with crypto.randomUUID answering `ids` (each a six-character game id) in order. */
async function withGameIds<T>(ids: () => string, body: () => Promise<T>): Promise<{ result: T; draws: number }> {
    const c = globalThis.crypto as { randomUUID: () => string };
    const real = c.randomUUID;
    let draws = 0;
    c.randomUUID = () => { draws++; return `${ids()}-0000-4000-8000-000000000000`.slice(0, 36); };
    try {
        const result = await body();
        await settle();
        return { result, draws };
    } finally {
        c.randomUUID = real;
    }
}

const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');

async function snapshot(gameId: string) {
    return {
        game: (await pgPool.query('SELECT version, state, roster, status FROM games WHERE id = $1', [gameId])).rows,
        members: (await pgPool.query('SELECT player_id FROM player_hands WHERE game_id = $1 ORDER BY 1', [gameId])).rows,
        views: (await pgPool.query('SELECT player_id, view, version FROM player_views WHERE game_id = $1 ORDER BY 1', [gameId])).rows,
        spectator: (await pgPool.query('SELECT view, version FROM spectator_views WHERE game_id = $1', [gameId])).rows,
    };
}

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });
after(async () => { await settle(); await pgPool.end(); });

test('create: a new id that collides with a stored game is drawn again, and the creator gets the game that was stored', async () => {
    const owner = uuid(), creator = uuid();
    await seedLobby(TAKEN, [{ id: owner, name: 'Owner', ready: false }]);
    await pgPool.query('INSERT INTO auth.users(id) VALUES ($1) ON CONFLICT DO NOTHING', [creator]);
    const before = await snapshot(TAKEN);
    const token = await tokenFor(creator, 'Creator');

    const script = [TAKEN, FREE];
    const { result: res, draws } = await withGameIds(() => {
        const id = script.shift();
        assert.ok(id, 'create drew more ids than the script holds');
        return id;
    }, () => postJson('create', token, {}));
    assert.equal(res.status, 200, JSON.stringify(res.json));

    // The response is the creator's view of a row that exists and seats them.
    const stored = await pgPool.query('SELECT game_id FROM player_views WHERE player_id = $1 AND view = $2', [creator, `\\x${hex(res.bytes)}`]);
    assert.deepEqual(stored.rows.map((r) => r.game_id), [FREE], 'the envelope create answered is the creator\'s stored view of the game it stored');
    const t = await mustReadTable(FREE);
    assert.deepEqual(t.seats.map((s) => s.id), [creator], 'the stored game seats the creator');
    assert.equal(t.version, 0);
    const table = fixtureTable();
    assert.equal(table.load(t.state, t.roster), L.TABLE_OK);
    assert.equal(hex(res.bytes), hex(table.envelope(FREE, 0, 0) as Uint8Array), 'the response is the kernel\'s envelope of the stored row');
    assert.equal(Buffer.from(res.bytes).includes(Buffer.from(TAKEN)), false, 'the response never names the taken id');
    assert.deepEqual((await pgPool.query('SELECT game_id FROM player_hands WHERE player_id = $1', [creator])).rows.map((r) => r.game_id), [FREE]);

    // The game that already held the id is untouched.
    assert.deepEqual(await snapshot(TAKEN), before, 'the colliding game keeps its row, members and views');
    assert.equal(draws, 2, 'one draw for the taken id, one for the free one');
});

test('create: when every id it draws is taken, it fails after a bounded number of draws and stores nothing', async () => {
    const owner = uuid(), creator = uuid();
    await seedLobby(TAKEN, [{ id: owner, name: 'Owner', ready: false }]);
    await pgPool.query('INSERT INTO auth.users(id) VALUES ($1) ON CONFLICT DO NOTHING', [creator]);
    const before = await snapshot(TAKEN);
    const token = await tokenFor(creator, 'Creator');

    const { result: res, draws } = await withGameIds(() => TAKEN, () => postJson('create', token, {}));
    assert.notEqual(res.status, 200, 'no game was stored, so none is handed out');
    assert.ok(draws > 1 && draws <= 8, `a few draws, then a clean error (drew ${draws})`);
    assert.deepEqual((await pgPool.query('SELECT game_id FROM player_hands WHERE player_id = $1', [creator])).rows, []);
    assert.deepEqual((await pgPool.query('SELECT game_id FROM player_views WHERE player_id = $1', [creator])).rows, []);
    assert.deepEqual(await snapshot(TAKEN), before, 'the colliding game keeps its row, members and views');
});
