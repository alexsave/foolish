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

        await runMeta(gameId, h1, { type: 'exit', game_id: gameId, bot_id: bot });
        let g = await loadCompleteGame(gameId);
        assert.equal(g.players.length, 1, 'bot removed');
        assert.equal((await pgPool.query('SELECT count(*) FROM bot_hands WHERE game_id=$1', [gameId])).rows[0].count, '0', 'bot hand deleted');

        await runMeta(gameId, h1, { type: 'exit', game_id: gameId }).catch(() => {});
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

    registerMetaValidation();

    after(async () => { await pgPool.end(); });
}
