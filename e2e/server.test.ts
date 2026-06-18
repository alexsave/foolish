// E2E: drives the REAL deployed server orchestration — executeWithGameLock (the
// optimistic-CAS retry loop), the real action handlers, commit_game / bot-lease
// plpgsql, and broadcastAnimationEvents — against a real Postgres. The only
// substitution is PostgREST/Realtime, replaced by the small pg adapter.

import './harness.ts'; // sets Deno globals BEFORE any server module loads
import { test, before, after, beforeEach } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, seedGame, uuid, pgPool, broadcastLog } from './harness.ts';
import { executeWithGameLock } from '../supabase/functions/_shared/utils.ts';
import { start_game } from '../supabase/functions/_shared/common_utils.ts';
import { AnimationEvent } from '../supabase/functions/_shared/types.ts';
import { legalMovesFor, applyPlayerMove, checkCardConservation, PlayerMove } from './dispatch.ts';

const rand = (n: number) => Math.floor(Math.random() * n);
const pick = <T>(a: T[]): T => a[rand(a.length)];

async function loadGame(gameId: string) {
    // use the real loader by going through executeWithGameLock would mutate; for a
    // read we just enumerate moves from a fresh load via the adapter-backed path.
    const { loadCompleteGame } = await import('../supabase/functions/_shared/utils.ts');
    return loadCompleteGame(gameId);
}

async function newGame(humans: number, bots: number): Promise<{ gameId: string; humanIds: string[]; botIds: Set<string> }> {
    const gameId = `g${uuid().slice(0, 6)}`;
    const players = [] as { id: string; name: string; is_ai: boolean; strategy_key: string }[];
    const humanIds: string[] = [];
    for (let i = 0; i < humans; i++) { const id = uuid(); humanIds.push(id); players.push({ id, name: `H${i}`, is_ai: false, strategy_key: 'human' }); }
    const botIds = new Set<string>();
    for (let i = 0; i < bots; i++) { const id = uuid(); botIds.add(id); players.push({ id, name: `B${i}`, is_ai: true, strategy_key: 'random' }); }
    await seedGame(gameId, players);
    await executeWithGameLock(gameId, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);
    return { gameId, humanIds, botIds };
}

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });

test('card conservation holds across a full sequential game (real executeWithGameLock + handlers + commit_game)', async () => {
    const { gameId } = await newGame(2, 1);
    let steps = 0;
    while (steps < 3000) {
        const g = await loadGame(gameId);
        if (g.status !== 'playing') break;
        const moves = legalMovesFor(g);
        if (moves.length === 0) break;
        const m = pick(moves);
        try { await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, m) }), `s${steps}`, true); } catch { /* stale */ }
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `card conservation violated at step ${steps}: ${chk.detail}`);
        steps++;
    }
    assert.ok(steps > 5, 'game should have progressed');
});

test('every broadcast carries a monotonically non-decreasing games.version (the reordering fix, server side)', async () => {
    const { gameId } = await newGame(2, 1);
    let steps = 0;
    while (steps < 2000) {
        const g = await loadGame(gameId);
        if (g.status !== 'playing') break;
        const moves = legalMovesFor(g);
        if (moves.length === 0) break;
        try { await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, pick(moves)) }), `s${steps}`, true); } catch { /* */ }
        steps++;
    }
    // Each animation_events broadcast must carry a numeric version; per recipient
    // channel, versions must be strictly increasing in emission order.
    const evts = broadcastLog.filter((b) => b.event === 'animation_events');
    assert.ok(evts.length > 0, 'expected broadcasts');
    const perChannel = new Map<string, number[]>();
    for (const e of evts) {
        assert.equal(typeof e.payload.version, 'number', 'broadcast payload must carry a version');
        if (!perChannel.has(e.channel)) perChannel.set(e.channel, []);
        perChannel.get(e.channel)!.push(e.payload.version);
    }
    for (const [chan, versions] of perChannel) {
        for (let i = 1; i < versions.length; i++) {
            assert.ok(versions[i] > versions[i - 1], `versions not strictly increasing on ${chan}: ${versions.join(',')}`);
        }
    }
});

test('CAS serializes concurrent moves without losing or duplicating a card', async () => {
    const { gameId, botIds } = await newGame(3, 1);
    let steps = 0;
    while (steps < 1500) {
        const snap = await loadGame(gameId);
        if (snap.status !== 'playing') break;
        const moves = legalMovesFor(snap, (id) => !botIds.has(id));
        if (moves.length === 0) { steps++; continue; }
        // fire a burst of overlapping requests against the same loaded version
        const burst: PlayerMove[] = [];
        const n = 1 + rand(Math.min(3, moves.length));
        for (let i = 0; i < n; i++) burst.push(pick(moves));
        if (Math.random() < 0.3) burst.push(burst[0]); // rapid double-submit
        await Promise.all(burst.map((m, i) => executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, m) }), `b${steps}-${i}`, true).catch(() => {})));
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `conservation broke under contention at step ${steps}: ${chk.detail}`);
        steps++;
    }
});

after(async () => { await pgPool.end(); });
