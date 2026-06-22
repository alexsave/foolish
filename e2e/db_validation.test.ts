// VALIDATION (Postgres-backed): small deterministic versions of the e2e tests
// whose invariants genuinely live in the database — the CAS commit, the bot-lease
// plpgsql, and the meta endpoint. These are NOT fuzzers: each plays a short,
// bounded sequence and asserts one invariant. Distilled from server.test.ts,
// lease.test.ts, meta.test.ts and concurrent_games.test.ts.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool, broadcastLog } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../supabase/functions/_shared/utils.ts';
import { handleMetaAction } from '../supabase/functions/_shared/meta_actions.ts';
import { start_game } from '../supabase/functions/_shared/common_utils.ts';
import { AnimationEvent, GAME_STATUS } from '../supabase/functions/_shared/types.ts';
import { legalMovesFor, applyPlayerMove, checkCardConservation, PlayerMove } from './dispatch.ts';

const pick = <T>(a: T[]): T => a[Math.floor(Math.random() * a.length)];

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });

async function startedGame(players: { id: string; name: string; is_ai: boolean; strategy_key: string }[]): Promise<string> {
    const gameId = `v${uuid().slice(0, 6)}`;
    await seedGame(gameId, players);
    await executeWithGameLock(gameId, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);
    return gameId;
}

// --- server.test.ts: card conservation + monotonic broadcast versions ---------
test('server: a short real game conserves cards and broadcasts strictly-increasing versions', async () => {
    const gameId = await startedGame([
        { id: uuid(), name: 'H0', is_ai: false, strategy_key: 'human' },
        { id: uuid(), name: 'H1', is_ai: false, strategy_key: 'human' },
        { id: uuid(), name: 'B0', is_ai: true, strategy_key: 'random' },
    ]);
    for (let step = 0; step < 40; step++) {
        const g = await loadCompleteGame(gameId);
        if (g.status !== 'playing') break;
        const moves = legalMovesFor(g);
        if (moves.length === 0) break;
        try { await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, pick(moves)) }), `s${step}`, true); } catch { /* stale */ }
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `card conservation violated at step ${step}: ${chk.detail}`);
    }
    // every animation broadcast carries a version; strictly increasing per channel.
    const evts = broadcastLog.filter((b) => b.event === 'animation_events');
    assert.ok(evts.length > 0, 'expected broadcasts');
    const perChannel = new Map<string, number[]>();
    for (const e of evts) {
        assert.equal(typeof e.payload.version, 'number', 'broadcast carries a numeric version');
        (perChannel.get(e.channel) ?? perChannel.set(e.channel, []).get(e.channel)!).push(e.payload.version);
    }
    for (const [chan, vs] of perChannel)
        for (let i = 1; i < vs.length; i++) assert.ok(vs[i] > vs[i - 1], `versions not increasing on ${chan}: ${vs.join(',')}`);
});

// --- server.test.ts / concurrent_games.test.ts: CAS under a rapid double-submit
test('server: a burst of overlapping submits against one version cannot duplicate or lose a card', async () => {
    const botIds = new Set<string>();
    const bot = uuid(); botIds.add(bot);
    const gameId = await startedGame([
        { id: uuid(), name: 'H0', is_ai: false, strategy_key: 'human' },
        { id: uuid(), name: 'H1', is_ai: false, strategy_key: 'human' },
        { id: bot, name: 'B0', is_ai: true, strategy_key: 'random' },
    ]);
    for (let step = 0; step < 25; step++) {
        const snap = await loadCompleteGame(gameId);
        if (snap.status !== 'playing') break;
        const moves = legalMovesFor(snap, (id) => !botIds.has(id));
        if (moves.length === 0) { continue; }
        const burst: PlayerMove[] = [pick(moves), pick(moves)];
        burst.push(burst[0]); // rapid double-submit of the same move
        await Promise.all(burst.map((m, i) => executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, m) }), `b${step}-${i}`, true).catch(() => {})));
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `conservation broke under contention at step ${step}: ${chk.detail}`);
    }
});

// --- concurrent_games.test.ts: isolation — independent games don't corrupt -----
test('concurrent: a few games progressing at once stay isolated (no deadlock, no cross-game corruption)', async () => {
    const ids = await Promise.all([0, 1, 2].map(() => startedGame([
        { id: uuid(), name: 'H', is_ai: false, strategy_key: 'human' },
        { id: uuid(), name: 'B', is_ai: true, strategy_key: 'random' },
    ])));
    const errors: string[] = [];
    await Promise.all(ids.map(async (gameId) => {
        for (let step = 0; step < 60; step++) {
            const g = await loadCompleteGame(gameId);
            if (g.status !== 'playing') break;
            const moves = legalMovesFor(g);
            if (moves.length === 0) break;
            try { await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, pick(moves)) }), `${gameId}-${step}`, true); }
            catch (e: any) { if (/deadlock/i.test(String(e.message))) errors.push(`DEADLOCK ${gameId}`); }
            const chk = await checkCardConservation(gameId);
            if (!chk.ok) errors.push(`${gameId}@${step}: ${chk.detail}`);
        }
    }));
    assert.deepEqual(errors, [], `cross-game errors: ${errors.slice(0, 5).join(' | ')}`);
});

// --- lease.test.ts: bot-lease plpgsql mutual exclusion / TTL / fencing ---------
const acquire = async (id: string, ttl: number) => (await pgPool.query('SELECT try_acquire_bot_lease($1,$2) AS t', [id, ttl])).rows[0].t;
const renew = async (id: string, tok: string, ttl: number) => (await pgPool.query('SELECT renew_bot_lease($1,$2,$3) AS r', [id, tok, ttl])).rows[0].r;
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

test('lease: exactly one concurrent acquire wins; TTL recovers; a stale token is fenced', async () => {
    const id = `l${uuid().slice(0, 6)}`;
    await seedGame(id, [{ id: uuid(), name: 'B', is_ai: true, strategy_key: 'random' }]);
    // mutual exclusion
    const tokens = await Promise.all(Array.from({ length: 12 }, () => acquire(id, 30_000)));
    assert.equal(tokens.filter((t) => t != null).length, 1, 'exactly one winner');

    // TTL recovery + stale fencing on a fresh game row
    const id2 = `l${uuid().slice(0, 6)}`;
    await seedGame(id2, [{ id: uuid(), name: 'B', is_ai: true, strategy_key: 'random' }]);
    const stale = await acquire(id2, 150);
    assert.ok(stale, 'acquired');
    assert.equal(await acquire(id2, 150), null, 'blocked while live');
    await sleep(300); // lapse
    const owner = await acquire(id2, 30_000);
    assert.ok(owner, 're-acquired after TTL');
    assert.equal(await renew(id2, owner, 30_000), true, 'owner renews');
    assert.equal(await renew(id2, stale, 30_000), false, 'stale token fenced');
});

// --- meta.test.ts: the consolidated meta endpoint ------------------------------
const runMeta = (gameId: string, userId: string, body: any) =>
    executeWithGameLock(gameId, async (g) => handleMetaAction({ user: { id: userId } as any, user_name: 'U', body, game: g, reqId: 'r' }), 'meta', false);

test('meta: start deals & conserves cards; an unknown action type is rejected', async () => {
    const h1 = uuid(), h2 = uuid();
    const gameId = `m${uuid().slice(0, 5)}`;
    await seedGame(gameId, [
        { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
        { id: h2, name: 'H2', is_ai: false, strategy_key: 'human' },
    ]); // seedGame marks players READY
    await runMeta(gameId, h1, { type: 'start', game_id: gameId });
    const g = await loadCompleteGame(gameId);
    assert.equal(g.status, GAME_STATUS.PLAYING, 'game started');
    assert.ok((await checkCardConservation(gameId)).ok, 'cards conserved on deal');

    await assert.rejects(runMeta(gameId, h1, { type: 'nonsense', game_id: gameId }), /unknown meta action/i, 'unknown rejected');
});

after(async () => { await pgPool.end(); });
