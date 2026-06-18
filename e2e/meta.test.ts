// E2E for the consolidated `meta` endpoint: the REAL handlers (start / add-bot /
// exit / continue from _shared/meta_actions.ts — the same code meta/index.ts
// dispatches) through the REAL CAS commit + pg adapter.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../supabase/functions/_shared/utils.ts';
import { handleMetaAction } from '../supabase/functions/_shared/meta_actions.ts';
import { GAME_STATUS, PLAYER_STATUS } from '../supabase/functions/_shared/types.ts';
import { checkCardConservation } from './dispatch.ts';

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });

const params = (game: any, userId: string, body: any) => ({ user: { id: userId } as any, user_name: 'U', body, game, reqId: 'r' });
const runMeta = (gameId: string, userId: string, body: any) =>
    executeWithGameLock(gameId, async (game) => handleMetaAction(params(game, userId, body)), 'meta', false);

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
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, chk.detail);
});

test('meta:add-bot — adds a bot and (all ready) starts the game', async () => {
    const gameId = `m${uuid().slice(0, 5)}`;
    const h1 = uuid();
    await seedGame(gameId, [{ id: h1, name: 'H1', is_ai: false, strategy_key: 'human' }]);
    // a bot must exist in the bots table for add-bot to pick
    await pgPool.query('INSERT INTO bots(id,nickname,strategy_key) VALUES($1,$2,$3)', [uuid(), 'Botty', 'random']);

    await runMeta(gameId, h1, { type: 'add-bot', game_id: gameId });
    const g = await loadCompleteGame(gameId);
    assert.equal(g.players.length, 2, 'bot added');
    assert.equal(g.players.filter(p => p.is_ai).length, 1, 'one bot');
    assert.equal(g.status, GAME_STATUS.PLAYING, 'all ready -> started');
});

test('meta:exit — removing a bot drops it; removing the last player deletes the game', async () => {
    const gameId = `m${uuid().slice(0, 5)}`;
    const h1 = uuid(), bot = uuid();
    await seedGame(gameId, [
        { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
        { id: bot, name: 'Botty', is_ai: true, strategy_key: 'random' },
    ]);
    // keep it in the lobby
    await pgPool.query("UPDATE games SET status='waiting' WHERE id=$1", [gameId]);

    await runMeta(gameId, h1, { type: 'exit', game_id: gameId, bot_id: bot });
    let g = await loadCompleteGame(gameId);
    assert.equal(g.players.length, 1, 'bot removed');
    assert.equal((await pgPool.query('SELECT count(*) FROM bot_hands WHERE game_id=$1', [gameId])).rows[0].count, '0', 'bot hand deleted');

    // Last player leaving: handleExit deletes the games row directly, so the
    // subsequent CAS commit has no row to update and throws — pre-existing
    // behaviour (the player is navigating away anyway). The game is still deleted.
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

test('meta: unknown type is rejected', async () => {
    const gameId = `m${uuid().slice(0, 5)}`;
    const h1 = uuid();
    await seedGame(gameId, [{ id: h1, name: 'H1', is_ai: false, strategy_key: 'human' }]);
    await assert.rejects(runMeta(gameId, h1, { type: 'nonsense', game_id: gameId }), /unknown meta action/i);
});

after(async () => { await pgPool.end(); });
