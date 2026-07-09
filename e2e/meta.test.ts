// E2E for the consolidated `meta` endpoint: the REAL handlers (start / add-bot /
// exit / continue from _shared/meta_actions.ts — the same code meta/index.ts
// dispatches) through the REAL CAS commit + pg adapter.
//
// Owns the meta validation scenarios; the fast runner
// (e2e/validation/db_validation.test.ts) imports `registerMetaValidation` and
// provides the shared DB before/after.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../supabase/functions/_shared/utils.ts';
import { handleMetaAction } from '../supabase/functions/_shared/meta_actions.ts';
import { GAME_STATUS, PLAYER_STATUS } from '../supabase/functions/_shared/types.ts';
import { checkCardConservation } from './dispatch.ts';

const params = (game: any, userId: string, body: any) => ({ user: { id: userId } as any, user_name: 'U', body, game, reqId: 'r' });
const runMeta = (gameId: string, userId: string, body: any) =>
    executeWithGameLock(gameId, async (game) => handleMetaAction(params(game, userId, body)), 'meta', false);

// ---- handpicked validation: a representative deal + a reject -----------------
export function registerMetaValidation(): void {
    test('meta:start — when all players are ready the game deals and conserves cards', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid(), h2 = uuid();
        await seedGame(gameId, [
            { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
            { id: h2, name: 'H2', is_ai: false, strategy_key: 'human' },
        ]); // seedGame marks players READY
        await runMeta(gameId, h1, { type: 'start', game_id: gameId });
        const g = await loadCompleteGame(gameId);
        assert.equal(g.status, GAME_STATUS.PLAYING, 'game started');
        assert.ok((await checkCardConservation(gameId)).ok, 'cards conserved on deal');
    });

    test('meta: unknown type is rejected', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid();
        await seedGame(gameId, [{ id: h1, name: 'H1', is_ai: false, strategy_key: 'human' }]);
        await assert.rejects(runMeta(gameId, h1, { type: 'nonsense', game_id: gameId }), /unknown meta action/i);
    });
}

if (!process.env.VALIDATION_ONLY) {
    before(async () => { await applySchema(); });
    beforeEach(async () => { await resetDb(); });

    test('meta:add-bot — adds a bot and (all ready) starts the game', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid();
        await seedGame(gameId, [{ id: h1, name: 'H1', is_ai: false, strategy_key: 'human' }]);
        await pgPool.query('INSERT INTO bots(id,nickname,strategy_key) VALUES($1,$2,$3)', [uuid(), 'Botty', 'random']);

        await runMeta(gameId, h1, { type: 'add-bot', game_id: gameId });
        const g = await loadCompleteGame(gameId);
        assert.equal(g.players.length, 2, 'bot added');
        assert.equal(g.players.filter(p => p.is_ai).length, 1, 'one bot');
        assert.equal(g.status, GAME_STATUS.PLAYING, 'all ready -> started');
    });

    test('meta:add-bot — a specific bot_id adds exactly that bot', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid(), b1 = uuid(), b2 = uuid();
        await seedGame(gameId, [{ id: h1, name: 'H1', is_ai: false, strategy_key: 'human' }]);
        await pgPool.query('INSERT INTO bots(id,nickname,strategy_key) VALUES($1,$2,$3),($4,$5,$6)',
            [b1, 'Botty1', 'random', b2, 'Botty2', 'random']);

        await runMeta(gameId, h1, { type: 'add-bot', game_id: gameId, bot_id: b2 });
        const g = await loadCompleteGame(gameId);
        const bots = g.players.filter(p => p.is_ai);
        assert.equal(bots.length, 1, 'one bot added');
        assert.equal(bots[0].player_id, b2, 'the requested bot (b2), not a random one');
    });

    test('meta:add-bot — an unavailable bot_id is rejected', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid();
        await seedGame(gameId, [{ id: h1, name: 'H1', is_ai: false, strategy_key: 'human' }]);
        await pgPool.query('INSERT INTO bots(id,nickname,strategy_key) VALUES($1,$2,$3)', [uuid(), 'Botty', 'random']);
        await assert.rejects(runMeta(gameId, h1, { type: 'add-bot', game_id: gameId, bot_id: uuid() }), /not available/i);
    });

    test('meta:exit — removing a bot drops it; removing the last player deletes the game', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid(), bot = uuid();
        await seedGame(gameId, [
            { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
            { id: bot, name: 'Botty', is_ai: true, strategy_key: 'random' },
        ]);
        await pgPool.query("UPDATE games SET status='waiting' WHERE id=$1", [gameId]);

        // seedGame gave the bot a bot_hands row; removing it must clear that row.
        // handleExit no longer DELETEs it directly — commit_game prunes bot_hands
        // not in the post-removal roster — so this asserts that prune fires.
        assert.equal((await pgPool.query('SELECT count(*) FROM bot_hands WHERE game_id=$1', [gameId])).rows[0].count, '1', 'bot hand present before removal');
        await runMeta(gameId, h1, { type: 'exit', game_id: gameId, bot_id: bot });
        let g = await loadCompleteGame(gameId);
        assert.equal(g.players.length, 1, 'bot removed');
        assert.equal((await pgPool.query('SELECT count(*) FROM bot_hands WHERE game_id=$1', [gameId])).rows[0].count, '0', 'bot hand pruned by commit_game');

        // The last exit must RESOLVE, not just happen to delete the row: it used
        // to succeed and then 400 (the CAS commit missed the deleted row, read it
        // as a conflict, and the retry's reload threw "not found").
        const res = await runMeta(gameId, h1, { type: 'exit', game_id: gameId });
        assert.equal(res.deleted, true, 'exit of the last player reports the deletion');
        assert.equal((await pgPool.query('SELECT count(*) FROM games WHERE id=$1', [gameId])).rows[0].count, '0', 'empty game deleted');
    });

    test('meta:continue — resets a finished game back to the lobby', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid(), h2 = uuid();
        await seedGame(gameId, [
            { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
            { id: h2, name: 'H2', is_ai: false, strategy_key: 'human' },
        ]);
        await pgPool.query("UPDATE games SET status='game_over' WHERE id=$1", [gameId]);

        await runMeta(gameId, h1, { type: 'continue', game_id: gameId });
        const g = await loadCompleteGame(gameId);
        assert.equal(g.status, GAME_STATUS.WAITING, 'reset to lobby');
        assert.ok(g.players.every(p => p.is_ai ? p.status === PLAYER_STATUS.READY : p.status === PLAYER_STATUS.IDLE), 'statuses reset');
    });

    test('meta:join — a new player joins a waiting game and gets a hand row', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid(), joiner = uuid();
        await seedGame(gameId, [{ id: h1, name: 'H1', is_ai: false, strategy_key: 'human' }]);
        await pgPool.query("UPDATE games SET status='waiting' WHERE id=$1", [gameId]);
        await pgPool.query('INSERT INTO auth.users(id) VALUES($1) ON CONFLICT DO NOTHING', [joiner]);

        await runMeta(gameId, joiner, { type: 'join', game_id: gameId });
        const g = await loadCompleteGame(gameId);
        assert.equal(g.players.length, 2, 'joiner added to players');
        assert.ok(g.players.some(p => p.player_id === joiner), 'joiner present');
        assert.equal((await pgPool.query('SELECT count(*) FROM player_hands WHERE game_id=$1 AND player_id=$2', [gameId, joiner])).rows[0].count, '1', 'joiner hand row persisted');

        await assert.rejects(runMeta(gameId, joiner, { type: 'join', game_id: gameId }), /already in game/i);
    });

    test('meta:rearrange-players — reorders the lobby seating', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid(), h2 = uuid();
        await seedGame(gameId, [
            { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
            { id: h2, name: 'H2', is_ai: false, strategy_key: 'human' },
        ]);
        await pgPool.query("UPDATE games SET status='waiting' WHERE id=$1", [gameId]);

        await runMeta(gameId, h1, { type: 'rearrange-players', game_id: gameId, new_order: [h2, h1] });
        const g = await loadCompleteGame(gameId);
        assert.deepEqual(g.players.map(p => p.player_id), [h2, h1], 'order swapped');

        await assert.rejects(runMeta(gameId, h1, { type: 'rearrange-players', game_id: gameId, new_order: [h1] }), /exactly 2 player/i);
        await assert.rejects(runMeta(gameId, h1, { type: 'rearrange-players', game_id: gameId, new_order: [h1, uuid()] }), /not found/i);
    });

    test('meta:update-name — renames the game in the lobby (with validation)', async () => {
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid();
        await seedGame(gameId, [{ id: h1, name: 'H1', is_ai: false, strategy_key: 'human' }]);
        await pgPool.query("UPDATE games SET status='waiting' WHERE id=$1", [gameId]);

        await runMeta(gameId, h1, { type: 'update-name', game_id: gameId, new_name: '  Cool Game  ' });
        const g = await loadCompleteGame(gameId);
        assert.equal(g.name, 'Cool Game', 'name trimmed + saved');

        await assert.rejects(runMeta(gameId, h1, { type: 'update-name', game_id: gameId, new_name: '   ' }), /non-empty/i);
        await assert.rejects(runMeta(gameId, h1, { type: 'update-name', game_id: gameId, new_name: 'x'.repeat(51) }), /50 characters/i);
    });

    test('create_game RPC — creates games + game_decks + player_hands in one call', async () => {
        const gameId = `c${uuid().slice(0, 5)}`;
        const creator = uuid();
        await pgPool.query('INSERT INTO auth.users(id) VALUES($1) ON CONFLICT DO NOTHING', [creator]);

        const players = [{ player_id: creator, name: 'Creator', status: 'idle', is_ai: false }];
        await pgPool.query('SELECT create_game($1,$2,$3,$4)', [gameId, "Creator's Game", creator, JSON.stringify(players)]);

        const g = await loadCompleteGame(gameId);
        assert.equal(g.status, GAME_STATUS.WAITING, 'waiting lobby');
        assert.equal(g.name, "Creator's Game");
        assert.equal(g.players.length, 1, 'creator seated');
        assert.deepEqual(g.deck, [], 'empty deck row created');
        assert.equal((await pgPool.query('SELECT count(*) FROM player_hands WHERE game_id=$1 AND player_id=$2', [gameId, creator])).rows[0].count, '1', 'creator hand row created');
    });

    // Regression: the full rematch cycle on a DEALT game (the seeded-continue
    // test above never writes a blob, so it misses the stale-blob class of
    // bug: `continue` used to leave the finished session's kernel blob in
    // games.state — COALESCE never cleared it — and the blob-authoritative
    // loaders then served the finished state to the new lobby: multi-human
    // rematches could never start, post-continue join/exit bricked every
    // load with a seat-count mismatch, and the old seats' hands leaked).
    test('meta:continue — full rematch on a dealt game: blob cleared, lobby mutable, restart works', async () => {
        const { legalMovesFor, applyPlayerMove } = await import('./dispatch.ts');
        const gameId = `m${uuid().slice(0, 5)}`;
        const h1 = uuid(), h2 = uuid(), h3 = uuid();
        await seedGame(gameId, [
            { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
            { id: h2, name: 'H2', is_ai: false, strategy_key: 'human' },
        ]);
        await runMeta(gameId, h1, { type: 'start', game_id: gameId });

        // Play the dealt game to completion so the final commit writes a
        // GAME_OVER blob — the exact state that used to go stale.
        for (let steps = 0; steps < 600; steps++) {
            const g = await loadCompleteGame(gameId);
            if (g.status !== GAME_STATUS.PLAYING) break;
            const moves = legalMovesFor(g);
            if (moves.length === 0) break;
            const pick = moves[Math.floor(Math.random() * moves.length)];
            try {
                await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, pick) }), `rm${steps}`, true);
            } catch { /* stale pick under the CAS — normal */ }
        }
        const finished = await pgPool.query('SELECT status, state FROM games WHERE id=$1', [gameId]);
        assert.equal(finished.rows[0].status, 'game_over', 'game played to completion');
        assert.ok(finished.rows[0].state, 'finished game carries a blob');

        // Continue: the reset commit must CLEAR the blob (state = NULL on a
        // WAITING transition), or everything below regresses.
        await runMeta(gameId, h1, { type: 'continue', game_id: gameId });
        const reset = await pgPool.query('SELECT status, state, logs_packed FROM games WHERE id=$1', [gameId]);
        assert.equal(reset.rows[0].status, 'waiting', 'reset to lobby');
        assert.equal(reset.rows[0].state, null, 'stale blob cleared on the WAITING transition');
        const lobbyG = await loadCompleteGame(gameId);
        assert.ok(lobbyG.players.every(p => p.hand.length === 0), 'no hands survive into the lobby');

        // The post-continue lobby must be fully mutable: join + exit used to
        // brick every subsequent load via the blob/roster seat mismatch.
        await pgPool.query('INSERT INTO auth.users(id) VALUES($1) ON CONFLICT DO NOTHING', [h3]);
        await runMeta(gameId, h3, { type: 'join', game_id: gameId });
        await runMeta(gameId, h2, { type: 'exit', game_id: gameId });
        const churned = await loadCompleteGame(gameId);
        assert.equal(churned.players.length, 2, 'join + exit both applied');
        assert.ok(churned.players.some(p => p.player_id === h3), 'joiner present');

        // Everyone readies up: the rematch must actually deal.
        await runMeta(gameId, h1, { type: 'start', game_id: gameId });
        await runMeta(gameId, h3, { type: 'start', game_id: gameId });
        const restarted = await loadCompleteGame(gameId);
        assert.equal(restarted.status, GAME_STATUS.PLAYING, 'rematch dealt');
        assert.ok((await checkCardConservation(gameId)).ok, 'cards conserved on the rematch deal');
    });

    registerMetaValidation();

    after(async () => { await pgPool.end(); });
}
