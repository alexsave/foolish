// A WAITING game never loads from a state blob.
//
// `continue` (meta_actions.ts handleContinue) returns a finished game to its
// lobby. The finished session's packed kernel blob (games.state) must not
// survive into that lobby, and nothing that reads a lobby may read through a
// blob that did survive (a row damaged before commit_game learned to clear it,
// or a column-only status change): trusting it leaks the previous session's
// hands and desyncs the blob's seats from the mutable lobby roster.
//
// The rule is enforced in FIVE places today, each on its own, so each has its
// own test here:
//
//   1. SQL commit_game          - a WAITING commit never keeps the finished
//                                 blob, even when the caller hands it one; the
//                                 games_legacy_bridge trigger (migration
//                                 20260917140000) writes the lobby blob of the
//                                 JSONB seats in its place
//   2. loadCompleteGame         - (adapter/utils.ts) a WAITING row assembles
//                                 from the membership rows, never the blob
//   3. buildPlayerViewRows      - (player_views.ts) a WAITING game's cached
//                                 per-player views are built without the blob
//   4. buildSpectatorView       - (player_views.ts) the same, for seat -1
//   5. buildPackedGameBytes     - (packed_game.ts) a WAITING row is not served
//                                 as a packed blob view
//
// Every test starts from the real path: a two-human game dealt and played to
// GAME_OVER through the real handlers and CAS commit, then `continue`d.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { commitGame, executeWithGameLock, loadCompleteGame } from '../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { handleMetaAction } from '../server/impls/supabase/functions/_shared/adapter/meta_actions.ts';
import { buildPlayerViewRows, buildSpectatorView } from '../server/api/common/player_views.ts';
import { buildPackedGameBytes } from '../server/api/common/packed_game.ts';
import { decodePackedGame } from '../sdk/ts/wire/view.ts';
import { hexToBytes } from '../server/api/common/replay/codec.ts';
import { GAME_STATUS, PersonalGame, PublicGame } from '../server/api/core/types.ts';
import { applyPlayerMove, legalMovesFor } from './dispatch.ts';
import { suiteRng } from './helpers/rng.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const rng = suiteRng('waiting_stale_blob');

const runMeta = (gameId: string, userId: string, body: object) =>
    executeWithGameLock(gameId, async (game) => handleMetaAction(
        { user: { id: userId } as never, user_name: 'U', body, game, reqId: 'r' } as never), 'meta', false);

interface Rematch { gameId: string; humans: string[]; staleState: string }

// Deal, play to GAME_OVER, `continue`. Returns the blob the finished game
// carried - the exact bytes that must not reach the lobby.
async function finishedThenContinued(): Promise<Rematch> {
    const gameId = `w${uuid().slice(0, 5)}`;
    const humans = [uuid(), uuid()];
    await seedGame(gameId, humans.map((id, i) => ({ id, name: `H${i}`, is_ai: false, strategy_key: 'human' })));
    await runMeta(gameId, humans[0], { type: 'start', game_id: gameId });
    for (let steps = 0; steps < 800; steps++) {
        const g = await loadCompleteGame(gameId);
        if (g.status !== GAME_STATUS.PLAYING) break;
        const moves = legalMovesFor(g);
        if (moves.length === 0) break;
        const pick = rng.pick(moves);
        try {
            await executeWithGameLock(gameId, async (gg) => ({ game: gg, ...applyPlayerMove(gg, pick) }), `s${steps}`, true);
        } catch { /* a stale pick under the CAS */ }
    }
    const done = await pgPool.query('SELECT status, state FROM games WHERE id=$1', [gameId]);
    assert.equal(done.rows[0].status, 'game_over', `game played to completion (seed=${rng.seed})`);
    assert.ok(done.rows[0].state, 'the finished game carries a blob');
    const staleState: string = done.rows[0].state;
    // The finished blob really does hold cards (the fool's hand), so a leak is visible.
    await runMeta(gameId, humans[0], { type: 'continue', game_id: gameId });
    return { gameId, humans, staleState };
}

// Put the finished session's blob back on the lobby row: a row damaged before
// commit_game cleared it, or a column-only status change.
const reinject = (gameId: string, state: string) =>
    pgPool.query('UPDATE games SET state=$1 WHERE id=$2', [state, gameId]);

// Every card a decoded view shows: own hand, every seat's count, the table,
// the deck count and the trump.
function cardsShown(view: PersonalGame | PublicGame): string[] {
    const shown: string[] = [];
    const self = (view as PersonalGame).self;
    if (self?.hand?.length) shown.push(`self.hand=${self.hand.length}`);
    for (const p of view.players) if (p.hand_length) shown.push(`${p.player_id}.hand_length=${p.hand_length}`);
    if (view.table_battles.length) shown.push(`table=${view.table_battles.length}`);
    if (view.deck_length) shown.push(`deck_length=${view.deck_length}`);
    if (view.flipped) shown.push('flipped');
    return shown;
}

if (!process.env.VALIDATION_ONLY) {
    before(async () => { await applySchema(); });
    beforeEach(async () => { await resetDb(); });

    test('commit_game: continue replaces the finished blob and clears the seed and session log', async () => {
        const { gameId } = await finishedThenContinued();
        const { rows } = await pgPool.query(
            'SELECT status, state, legacy_lobby_state_hex(players) AS lobby, game_seed, logs_packed FROM games WHERE id=$1', [gameId]);
        assert.equal(rows[0].status, 'waiting');
        assert.equal(rows[0].state, rows[0].lobby, 'no finished blob survives into the lobby: the lobby blob of its seats does');
        assert.equal(rows[0].game_seed, null, 'no deal seed survives into the lobby');
        assert.equal(rows[0].logs_packed, '', 'no session log survives into the lobby');
    });

    test('commit_game: a WAITING commit handed a blob persists only the lobby blob of its seats', async () => {
        const { gameId, staleState } = await finishedThenContinued();
        const lobby = await loadCompleteGame(gameId);
        const res = await commitGame(lobby, lobby.version, staleState);
        assert.equal(res.status, 'ok', 'the lobby commit lands');
        const { rows } = await pgPool.query(
            'SELECT status, state, legacy_lobby_state_hex(players) AS lobby FROM games WHERE id=$1', [gameId]);
        assert.equal(rows[0].status, 'waiting');
        assert.notEqual(rows[0].state, staleState, 'commit_game refused to persist the handed blob for a WAITING game');
        assert.equal(rows[0].state, rows[0].lobby, 'the row holds the lobby blob of its seats');
    });

    test('loadCompleteGame: a WAITING row that still carries a blob loads as a lobby', async () => {
        const { gameId, humans, staleState } = await finishedThenContinued();
        await reinject(gameId, staleState);
        const g = await loadCompleteGame(gameId);
        assert.equal(g.status, GAME_STATUS.WAITING);
        assert.deepEqual(g.players.map(p => p.player_id).sort(), [...humans].sort(), 'the lobby roster');
        assert.deepEqual(g.players.map(p => p.hand.length), [0, 0], 'no hand from the finished session');
        assert.deepEqual(g.deck, [], 'no deck from the finished session');
        assert.equal(g.table_battles.length, 0, 'no table from the finished session');
        assert.equal(g.flipped, null, 'no trump from the finished session');
    });

    test('buildPlayerViewRows: a WAITING game handed a blob caches views with no cards', async () => {
        const { gameId, humans, staleState } = await finishedThenContinued();
        const lobby = await loadCompleteGame(gameId);
        const rows = await buildPlayerViewRows(lobby, staleState, lobby.version + 1);
        assert.equal(rows.length, humans.length, 'one row per human');
        for (const row of rows) {
            const decoded = decodePackedGame(hexToBytes(row.view));
            assert.ok(decoded, 'the row decodes');
            assert.deepEqual(cardsShown(decoded!.game), [], `${row.player_id}'s lobby view shows no old cards`);
        }
    });

    test('player_views rows written by the real continue commit carry no cards', async () => {
        const { gameId } = await finishedThenContinued();
        const { rows } = await pgPool.query('SELECT player_id, view, status FROM player_views WHERE game_id=$1', [gameId]);
        assert.equal(rows.length, 2, 'both humans have a row');
        for (const r of rows) {
            assert.equal(r.status, 'waiting');
            const decoded = decodePackedGame(hexToBytes(r.view));
            assert.deepEqual(cardsShown(decoded!.game), [], `${r.player_id}'s lobby row shows no old cards`);
        }
    });

    test('buildSpectatorView: a WAITING game handed a blob shows spectators no cards', async () => {
        const { gameId, staleState } = await finishedThenContinued();
        const lobby = await loadCompleteGame(gameId);
        const hex = await buildSpectatorView(lobby, staleState, lobby.version + 1);
        const decoded = decodePackedGame(hexToBytes(hex));
        assert.ok(decoded, 'the spectator view decodes');
        assert.deepEqual(cardsShown(decoded!.game), [], 'the spectator lobby view shows no old cards');
    });

    test('buildPackedGameBytes: a WAITING row that still carries a blob is not served from it', async () => {
        const { gameId, humans, staleState } = await finishedThenContinued();
        await reinject(gameId, staleState);
        const { rows } = await pgPool.query(
            'SELECT id, name, status, version, state, players, good_players, good_timestamp FROM games WHERE id=$1', [gameId]);
        assert.equal(rows[0].status, 'waiting');
        assert.ok(rows[0].state, 'the row really carries the stale blob');
        assert.equal(await buildPackedGameBytes(rows[0], humans[0]), null, 'a lobby is never served from a blob');
    });

    after(async () => { await pgPool.end(); });
}
