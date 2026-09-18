// Account deletion scrubs the name from every cached view, not only the roster
// (docs/C_GAME_SHAPE_MIGRATION.md Q8, Phase 4b).
//
// The envelopes in player_views.view and spectator_views.view carry every seat's
// name in their roster trailer. The delete_account SQL only ever rewrote the
// JSONB roster, so the cached views of a deleted user's games kept the real name
// for as long as nothing else committed. Here a lobby and a dealt game are made
// through the real edge functions, the user deletes their account through the
// real delete-account function, and no view, roster or trailer of those games
// may still hold the name's bytes.
//
// A title is the table's, not a seat's: the first test gives both tables a title
// of their own. The second holds the one title that IS the name: the default
// "<name>'s Game" table_create composed, which table_redact recomposes from the
// replacement name, while a title somebody chose stays as it is.

import './harness.ts';
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, pgPool, uuid } from './harness.ts';
import { postJson, settle, tokenFor } from './helpers/edge.ts';
import { mustReadTable } from './helpers/table_play.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

before(async () => { await applySchema(); });
after(async () => { await settle(); });

const NAME = 'Zeldaquist Vanishing';
const nameHex = Buffer.from(NAME, 'utf8').toString('hex');

async function gameIdOf(createResponse: Uint8Array): Promise<string> {
    await settle();
    const { rows } = await pgPool.query('SELECT game_id FROM player_views WHERE view = $1', [`\\x${Buffer.from(createResponse).toString('hex')}`]);
    assert.equal(rows.length, 1, 'the created lobby has its creator view');
    return rows[0].game_id;
}

async function holdingName(gameIds: string[]): Promise<string[]> {
    const where: string[] = [];
    for (const g of gameIds) {
        const pv = await pgPool.query('SELECT player_id, view FROM player_views WHERE game_id = $1', [g]);
        for (const r of pv.rows) if (String(r.view).includes(nameHex)) where.push(`${g} player_views(${r.player_id})`);
        const sv = await pgPool.query('SELECT view FROM spectator_views WHERE game_id = $1', [g]);
        for (const r of sv.rows) if (String(r.view).includes(nameHex)) where.push(`${g} spectator_views`);
        const gr = await pgPool.query('SELECT roster FROM games WHERE id = $1', [g]);
        for (const r of gr.rows) if (String(r.roster).includes(nameHex)) where.push(`${g} games.roster`);
    }
    return where;
}

test('after delete-account, no view or roster of the user\'s games holds their name', async () => {
    const A = uuid(), B = uuid();
    await pgPool.query('INSERT INTO auth.users(id) VALUES ($1), ($2)', [A, B]);
    const tokA = await tokenFor(A, NAME), tokB = await tokenFor(B, 'Bystander');

    // A lobby A made and B joined.
    const lobbyRes = await postJson('create', tokA, {});
    assert.equal(lobbyRes.status, 200, JSON.stringify(lobbyRes.json));
    const lobby = await gameIdOf(lobbyRes.bytes);
    assert.equal((await postJson('meta', tokB, { type: 'join', game_id: lobby })).status, 200);
    // The default title is "<creator>'s Game"; a title is the table's, not a seat's
    // name, so this test gives both tables their own (see the note at the top).
    assert.equal((await postJson('meta', tokB, { type: 'update-name', game_id: lobby, new_name: 'Lobby table' })).status, 200);

    // A game they dealt.
    const dealtRes = await postJson('create', tokA, {});
    const dealt = await gameIdOf(dealtRes.bytes);
    assert.equal((await postJson('meta', tokB, { type: 'join', game_id: dealt })).status, 200);
    assert.equal((await postJson('meta', tokB, { type: 'update-name', game_id: dealt, new_name: 'Dealt table' })).status, 200);
    for (const tok of [tokA, tokB]) assert.equal((await postJson('meta', tok, { type: 'start', game_id: dealt })).status, 200);
    await settle();
    assert.equal((await mustReadTable(dealt)).statusColumn, 'playing', 'fixture: the second game dealt');

    const games = [lobby, dealt];
    assert.ok((await holdingName(games)).length >= 6, 'fixture: the name is in the views and rosters before deletion');

    const res = await postJson('delete-account', tokA, {});
    assert.equal(res.status, 200, JSON.stringify(res.json));
    await settle();

    assert.deepEqual(await holdingName(games), [], 'no cached view, spectator view or roster holds the name');
    for (const g of games) {
        const t = await mustReadTable(g);
        assert.equal(t.seats[0].id, A, 'the seat is still the deleted user\'s');
        assert.equal(t.seats[0].name, 'Deleted player', 'renamed in the roster');
        const { rows } = await pgPool.query('SELECT view FROM player_views WHERE game_id = $1 AND player_id = $2', [g, B]);
        assert.ok(String(rows[0].view).includes(Buffer.from('Deleted player').toString('hex')), `B's cached view of ${g} shows the redaction`);
    }
    assert.equal((await pgPool.query('SELECT 1 FROM auth.users WHERE id = $1', [A])).rows.length, 0, 'the auth user is gone');
});

test('after delete-account, a table still titled "<name>\'s Game" is retitled from the replacement name', async () => {
    const A = uuid(), B = uuid();
    await pgPool.query('INSERT INTO auth.users(id) VALUES ($1), ($2)', [A, B]);
    const tokA = await tokenFor(A, NAME), tokB = await tokenFor(B, 'Bystander');
    const res = await postJson('create', tokA, {});
    assert.equal(res.status, 200, JSON.stringify(res.json));
    const gameId = await gameIdOf(res.bytes);
    assert.equal((await postJson('meta', tokB, { type: 'join', game_id: gameId })).status, 200);
    assert.equal((await mustReadTable(gameId)).title, `${NAME}'s Game`, 'fixture: the default title holds the name');
    assert.ok((await holdingName([gameId])).length >= 3, 'fixture: so every view and the roster hold it');

    assert.equal((await postJson('delete-account', tokA, {})).status, 200);
    await settle();

    assert.deepEqual(await holdingName([gameId]), [], 'no view, spectator view or roster holds the name, title included');
    assert.equal((await mustReadTable(gameId)).title, "Deleted player's Game", 'the default title follows the redacted name');
});
