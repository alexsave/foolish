// A lobby never carries a finished session's board.
//
// `continue` returns a finished game to its lobby. The finished session's state
// blob must not survive into that lobby: trusting it leaks the previous
// session's hands and desyncs the board's seats from the mutable lobby roster.
//
// Before Phase 4b of docs/C_GAME_SHAPE_MIGRATION.md that rule was enforced in
// five places, each with its own test here: the commit_game SQL, and four TS
// readers (loadCompleteGame, buildPlayerViewRows, buildSpectatorView,
// buildPackedGameBytes) that each refused to read a WAITING row through its
// blob. The TS readers are gone with the TS game shape, and the rule is now one
// kernel check plus the writer: game_validate refuses a WAITING board holding
// any card (GAME_INVALID_LOBBY_CARDS, pinned by kernel_state_validation.test.ts
// and the C tests), table_continue writes the lobby board, and a stored stale
// blob is read as the finished game it is, never as a lobby (the table_parity
// fixture row, pinned in table_parity.test.ts and table_expand_migration.test.ts).
//
// What stays here is the real path end to end: a two-human game dealt and played
// to GAME_OVER through the real handlers and commit_table, then `continue`d, and
// every stored product of that commit compared with the products of a lobby of
// the same seats that never held a card.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixture, fixtureTable } from './helpers/table_fixture.ts';
import { mustReadTable } from './helpers/table_play.ts';
import { playToEnd, runMeta, seedLobby } from './helpers/table_server.ts';
import { suiteRng } from './helpers/rng.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const rng = suiteRng('waiting_stale_blob');

interface Rematch { gameId: string; humans: string[]; finishedState: Uint8Array }

// Deal, play to GAME_OVER, `continue`. Returns the blob the finished game
// carried: the exact bytes that must not reach the lobby.
async function finishedThenContinued(): Promise<Rematch> {
    const gameId = `w${uuid().slice(0, 5)}`;
    const humans = [uuid(), uuid()];
    await seedLobby(gameId, humans.map((id, i) => ({ id, name: `H${i}`, ready: i > 0 })));
    await runMeta(gameId, humans[0], { type: 'start' });
    const done = await playToEnd(gameId, { pick: (m) => rng.pick(m) });
    assert.equal(done.statusColumn, 'game_over', `game played to completion (seed=${rng.seed})`);
    assert.ok(done.seats.some((s) => s.hand.length > 0), 'the finished board really holds cards (the fool\'s hand), so a leak is visible');
    await runMeta(gameId, humans[0], { type: 'continue' });
    return { gameId, humans, finishedState: done.state };
}

const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');

if (!process.env.VALIDATION_ONLY) {
    before(async () => { await applySchema(); });
    beforeEach(async () => { await resetDb(); });

    test('continue writes the lobby board of the seats, and clears the deal seed and the session log', async () => {
        const { gameId, humans, finishedState } = await finishedThenContinued();
        const t = await mustReadTable(gameId);
        assert.equal(t.statusColumn, 'waiting');
        assert.notEqual(hex(t.state), hex(finishedState), 'the finished blob is gone');
        // A lobby of the same seats and title that never held a card.
        const clean = fixture().title(t.title).seats(humans.map((id, i) => ({ id, name: `H${i}` }))).build();
        assert.equal(hex(t.state), hex(clean.state), 'the row holds exactly the lobby board of its seats');
        assert.equal(hex(t.roster), hex(clean.roster), 'and the same roster');
        const { rows } = await pgPool.query('SELECT game_seed, logs_packed FROM games WHERE id=$1', [gameId]);
        assert.equal(rows[0].game_seed, null, 'no deal seed survives into the lobby');
        assert.equal(rows[0].logs_packed, '', 'no session log survives into the lobby');
    });

    test('the player_views and spectator_views rows the continue commit wrote are a clean lobby\'s envelopes', async () => {
        const { gameId, humans } = await finishedThenContinued();
        const t = await mustReadTable(gameId);
        const clean = fixture().title(t.title).seats(humans.map((id, i) => ({ id, name: `H${i}` }))).build();
        const table = fixtureTable();
        assert.equal(table.load(clean.state, clean.roster), L.TABLE_OK);

        const { rows } = await pgPool.query('SELECT player_id, view, version, status FROM player_views WHERE game_id=$1 ORDER BY player_id', [gameId]);
        assert.equal(rows.length, 2, 'both humans have a row');
        for (const r of rows) {
            assert.equal(r.status, 'waiting');
            assert.equal(Number(r.version), t.version, 'written by the continue commit');
            const want = table.envelope(gameId, humans.indexOf(r.player_id), t.version);
            assert.ok(want instanceof Uint8Array);
            assert.equal(r.view, hex(want), `${r.player_id}'s lobby row is a clean lobby's envelope: no old card, count or trump`);
        }
        const spec = await pgPool.query('SELECT view, status FROM spectator_views WHERE game_id=$1', [gameId]);
        const want = table.envelope(gameId, -1, t.version);
        assert.ok(want instanceof Uint8Array);
        assert.equal(spec.rows[0].status, 'waiting');
        assert.equal(spec.rows[0].view, hex(want), 'the spectator row is a clean lobby\'s envelope');
    });

    test('a finished blob put back on a WAITING row is read as the finished game, never as a lobby', async () => {
        const { gameId, humans, finishedState } = await finishedThenContinued();
        // A row damaged by a column-only status change.
        await pgPool.query('UPDATE games SET state=$1 WHERE id=$2', [`\\x${hex(finishedState)}`, gameId]);
        // A lobby edit on it is refused: the board says the game is over, so it is not a lobby.
        await assert.rejects(runMeta(gameId, humans[1], { type: 'update-name', new_name: 'x' }), /not in its lobby/i);
        // The same board relabelled WAITING in the blob itself does not load at all.
        const relabelled = finishedState.slice();
        relabelled[2] = L.GAME_STATUS_WAITING;
        const t = await mustReadTable(gameId);
        assert.equal(fixtureTable().load(relabelled, t.roster), L.GAME_INVALID_LOBBY_CARDS, 'a lobby holding cards is refused');
    });

    after(async () => { await pgPool.end(); });
}
