// player_views - the server-written, client-read personalized view cache
// (docs/PLAYER_VIEWS.md). These prove the WRITE side end-to-end against real
// Postgres running the real commit_table / create_table plpgsql:
//
//  1. A dealt commit writes one row per human (none for bots), stamped with the
//     committed version and status, byte-identical to the envelope the kernel
//     writes for that seat from the stored row (table_envelope: the masking is
//     view.c state_put's, held by the S1 suite and the C tests).
//  2. create seeds the creator's lobby row.
//  3. Exiting prunes the leaver's row (they stop seeing the game in their list).
//  4. RLS: a player reads ONLY their own rows; no client can write.
//
// And the sibling spectator_views cache (the SHARED seat -1 view):
//  5. A dealt commit / create writes the spectator row, byte-identical to the
//     kernel's seat -1 envelope of the stored row.
//  6. RLS: ANY authenticated user may read it; no client can write.
//
// Retired with the TS view builders (Phase 4b): the backfill case for
// buildPlayerViewUpserts (it had no runtime caller; that a cached view can be
// rebuilt byte for byte from the row is what cases 1 and 5 assert against the
// kernel) and the decodePackedGame reads of the rows (what a decoder shows a
// viewer is security_hidden_info.test.ts's subject).

import './harness.ts';
import { test, before, beforeEach } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { legalMoves, mustReadTable, type TableState } from './helpers/table_play.ts';
import { runAction, runMeta, seedLobby } from './helpers/table_server.ts';
import { postJson, settle, tokenFor } from './helpers/edge.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

async function viewsFor(gameId: string): Promise<Map<string, { view: string; version: string; status: string }>> {
    const { rows } = await pgPool.query(
        'SELECT player_id, view, version, status FROM player_views WHERE game_id=$1', [gameId]);
    const m = new Map<string, { view: string; version: string; status: string }>();
    for (const r of rows) m.set(r.player_id, { view: r.view, version: String(r.version), status: r.status });
    return m;
}

async function spectatorFor(gameId: string): Promise<{ view: string; version: string; status: string } | null> {
    const { rows } = await pgPool.query(
        'SELECT view, version, status FROM spectator_views WHERE game_id=$1', [gameId]);
    if (rows.length === 0) return null;
    return { view: rows[0].view, version: String(rows[0].version), status: rows[0].status };
}

/** The envelope the kernel writes for `viewer` (a seat, or -1) from the stored row. */
function envelopeOf(t: TableState, viewer: number): string {
    const table = fixtureTable();
    assert.equal(table.load(t.state, t.roster), L.TABLE_OK);
    const e = table.envelope(t.gameId, viewer, t.version);
    assert.ok(e instanceof Uint8Array, `table_envelope(${viewer}) = ${e}`);
    return `\\x${Buffer.from(e).toString('hex')}`;   // a BYTEA view, as PostgREST and the pool read it
}

async function dealtGame(prefix: string, bot = false): Promise<{ gameId: string; h1: string; h2: string; b1: string }> {
    const gameId = `${prefix}${uuid().slice(0, 5)}`;
    const h1 = uuid(), h2 = uuid(), b1 = uuid();
    await seedLobby(gameId, [
        { id: h1, name: 'H1', ready: false }, { id: h2, name: 'H2' },
        ...(bot ? [{ id: b1, name: '%Bot', brain: 'random' }] : []),
    ]);
    await runMeta(gameId, h1, { type: 'start' });
    return { gameId, h1, h2, b1 };
}

before(async () => {
    await applySchema();
    // Make the harness's stub auth.uid() honor a per-connection JWT claim, so the
    // RLS SELECT policy (player_id = auth.uid()) is actually exercised.
    await pgPool.query(`
      CREATE OR REPLACE FUNCTION auth.uid() RETURNS uuid LANGUAGE sql STABLE AS $$
        SELECT NULLIF(current_setting('request.jwt.claims', true)::jsonb->>'sub','')::uuid
      $$;`);
    // Likewise a per-connection role claim, so the spectator_views policy
    // (auth.role() = 'authenticated') is exercised; the default stays
    // 'service_role' for every other test.
    await pgPool.query(`
      CREATE OR REPLACE FUNCTION auth.role() RETURNS text LANGUAGE sql STABLE AS $$
        SELECT COALESCE(NULLIF(current_setting('request.jwt.claims', true)::jsonb->>'role',''), 'service_role')
      $$;`);
});
beforeEach(async () => { await resetDb(); });

test('dealt commit writes one row per human (not bots), each the kernel\'s envelope for that seat', async () => {
    const { gameId, h1, h2, b1 } = await dealtGame('p', true);
    const t = await mustReadTable(gameId);
    assert.equal(t.statusColumn, 'playing', 'game dealt');

    const views = await viewsFor(gameId);
    assert.equal(views.size, 2, 'one row per human, none for the bot');
    assert.ok(views.has(h1) && views.has(h2), 'both humans have a row');
    assert.ok(!views.has(b1), 'bot has no row');
    for (const [seat, pid] of [[0, h1], [1, h2]] as const) {
        const row = views.get(pid)!;
        assert.equal(row.status, 'playing', 'status denormalized');
        assert.equal(row.version, String(t.version), 'version mirrors games.version');
        assert.equal(row.view, envelopeOf(t, seat), `cached view == the kernel's envelope for seat ${seat}`);
        assert.notEqual(row.view, envelopeOf(t, -1), 'and not the spectator\'s: it carries the owner\'s hand');
    }
});

test('a move refreshes each human\'s cached view (version bumped, still the kernel\'s envelope)', async () => {
    const { gameId, h1, h2 } = await dealtGame('m');
    const before = await viewsFor(gameId);
    const t0 = await mustReadTable(gameId);
    const move = legalMoves(t0)[0];
    assert.equal((await runAction(gameId, move.playerId, move)).status, 0, 'the move applied');

    const t = await mustReadTable(gameId);
    const after = await viewsFor(gameId);
    assert.equal(after.size, 2, 'still one row per human after the move');
    for (const [seat, pid] of [[0, h1], [1, h2]] as const) {
        assert.ok(Number(after.get(pid)!.version) > Number(before.get(pid)!.version), `version bumped for ${pid}`);
        assert.equal(after.get(pid)!.version, String(t.version), 'and mirrors the new games.version');
        assert.equal(after.get(pid)!.view, envelopeOf(t, seat), `refreshed cache == the kernel's envelope for ${pid}`);
    }
});

// #24 / #15 desync: the end-of-game row reaches a client only over a
// fire-and-forget realtime push; lose it and the client is stranded on the table
// (WinView never shown). The client fix is a plain refetch on feed-silence, and a
// refetch is exactly `viewsFor`. So the fix is sound iff, once a game ends, EVERY
// human already has a TERMINAL player_views row.
test('#24: at game-over every human has a terminal player_views row a refetch retrieves', async () => {
    const { gameId, h1, h2 } = await dealtGame('e');
    for (let i = 0; i < 2000; i++) {
        const t = await mustReadTable(gameId);
        if (t.status !== L.GAME_STATUS_PLAYING) break;
        const moves = legalMoves(t);
        assert.ok(moves.length > 0, 'a running game has a move');
        await runAction(gameId, moves[0].playerId, moves[0]);
    }
    const t = await mustReadTable(gameId);
    assert.equal(t.statusColumn, 'game_over', 'the driven game reached game-over');
    assert.ok(t.eliminated.length >= 1, 'the finish is settled on the board');

    const views = await viewsFor(gameId);
    assert.equal(views.size, 2, 'a terminal row for EACH human (the server writes all seats)');
    for (const [seat, pid] of [[0, h1], [1, h2]] as const) {
        const row = views.get(pid)!;
        assert.equal(row.status, 'game_over', `${pid} row denormalized game_over`);
        assert.equal(row.version, String(t.version), `${pid} row is the terminal commit's`);
        assert.equal(row.view, envelopeOf(t, seat), `${pid} refetch is the terminal envelope`);
    }
});

test('create seeds the creator\'s lobby row and the spectator lobby row', async () => {
    const h1 = uuid();
    await pgPool.query('INSERT INTO auth.users(id) VALUES($1)', [h1]);
    const res = await postJson('create', await tokenFor(h1, 'H1'), {});
    assert.equal(res.status, 200, JSON.stringify(res.json));
    await settle();
    const { rows } = await pgPool.query('SELECT game_id FROM player_views WHERE player_id=$1', [h1]);
    assert.equal(rows.length, 1, 'creator row seeded');
    const gameId = rows[0].game_id;
    const t = await mustReadTable(gameId);

    const views = await viewsFor(gameId);
    assert.equal(views.get(h1)!.status, 'waiting');
    assert.equal(views.get(h1)!.version, '0');
    assert.equal(views.get(h1)!.view, envelopeOf(t, 0), 'the creator\'s row is the kernel\'s seat 0 envelope');
    assert.equal(views.get(h1)!.view, `\\x${Buffer.from(res.bytes).toString('hex')}`, 'and the create response body');

    const spec = await spectatorFor(gameId);
    assert.ok(spec, 'spectator lobby row seeded');
    assert.equal(spec!.status, 'waiting');
    assert.equal(spec!.view, envelopeOf(t, -1), 'the spectator row is the kernel\'s seat -1 envelope');
});

test('exiting a game prunes the leaver\'s view row', async () => {
    const gameId = `x${uuid().slice(0, 5)}`;
    const h1 = uuid(), h2 = uuid();
    await seedLobby(gameId, [{ id: h1, name: 'H1', ready: false }, { id: h2, name: 'H2', ready: false }]);

    // A lobby commit (rename) populates a row for BOTH humans.
    await runMeta(gameId, h1, { type: 'update-name', new_name: 'Renamed' });
    let views = await viewsFor(gameId);
    assert.ok(views.has(h1) && views.has(h2), 'both humans have a row after a lobby commit');

    // H2 exits: the commit's participant set no longer includes them.
    await runMeta(gameId, h2, { type: 'exit', player_id: h2 });
    views = await viewsFor(gameId);
    assert.ok(views.has(h1), 'remaining player keeps their row');
    assert.ok(!views.has(h2), 'the leaver\'s row is pruned');
});

test('RLS: a player reads only their own rows, and clients cannot write', async () => {
    const { gameId, h1 } = await dealtGame('s');
    const c = await pgPool.connect();
    try {
        await c.query('BEGIN');
        await c.query('SET LOCAL ROLE authenticated');
        await c.query(`SELECT set_config('request.jwt.claims', json_build_object('sub', $1::text)::text, true)`, [h1]);
        const mine = await c.query('SELECT player_id FROM player_views');
        assert.equal(mine.rows.length, 1, 'H1 sees one row');
        assert.equal(mine.rows[0].player_id, h1, 'and it is their own');
        await assert.rejects(
            c.query(`INSERT INTO player_views(game_id,player_id,view,version,status) VALUES($1,$2,'00',0,'playing')`, [gameId, h1]),
            /permission denied/, 'authenticated cannot insert');
        await c.query('ROLLBACK');
    } finally {
        c.release();
    }
});

test('a dealt commit writes the spectator row, the kernel\'s seat -1 envelope of the stored row', async () => {
    const { gameId } = await dealtGame('sp');
    const t = await mustReadTable(gameId);
    const spec = await spectatorFor(gameId);
    assert.ok(spec, 'a spectator row was written');
    assert.equal(spec!.status, 'playing', 'status denormalized');
    assert.equal(spec!.version, String(t.version), 'version mirrors games.version');
    assert.equal(spec!.view, envelopeOf(t, -1), 'cached spectator view == the kernel\'s spectator envelope');
    for (const seat of [0, 1]) assert.notEqual(spec!.view, envelopeOf(t, seat), `and not seat ${seat}'s`);
});

test('RLS: ANY authenticated user can read a spectator row, and clients cannot write', async () => {
    const { gameId } = await dealtGame('sr');
    const outsider = uuid();
    const c = await pgPool.connect();
    try {
        await c.query('BEGIN');
        await c.query('SET LOCAL ROLE authenticated');
        await c.query(`SELECT set_config('request.jwt.claims', json_build_object('sub', $1::text, 'role', 'authenticated')::text, true)`, [outsider]);
        const seen = await c.query('SELECT game_id FROM spectator_views WHERE game_id=$1', [gameId]);
        assert.equal(seen.rows.length, 1, 'a non-participant can read the spectator row');
        await assert.rejects(
            c.query(`INSERT INTO spectator_views(game_id,view,version,status) VALUES($1,'00',0,'playing')`, [`${gameId}x`]),
            /permission denied/, 'authenticated cannot insert');
        await c.query('ROLLBACK');
    } finally {
        c.release();
    }
});
