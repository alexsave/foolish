// E2E: drives the REAL deployed server orchestration — executeWithGameLock (the
// optimistic-CAS retry loop), the real action handlers, commit_game / bot-lease
// plpgsql, and broadcastAnimationEvents — against a real Postgres. The only
// substitution is PostgREST/Realtime, replaced by the small pg adapter.

import './harness.ts'; // sets Deno globals BEFORE any server module loads
import { test, before, after, beforeEach } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, seedGame, uuid, pgPool, broadcastLog } from './harness.ts';
import { executeWithGameLock } from '../supabase/functions/_shared/utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
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

// ---- handpicked validation: a short game + a burst, sharing DB before/after ---
export function registerServerValidation(): void {
    test('server: a short real game conserves cards and broadcasts strictly-increasing versions', async () => {
        const { gameId } = await newGame(2, 1);
        for (let step = 0; step < 40; step++) {
            const g = await loadGame(gameId);
            if (g.status !== 'playing') break;
            const moves = legalMovesFor(g);
            if (moves.length === 0) break;
            try { await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, pick(moves)) }), `s${step}`, true); } catch { /* stale */ }
            const chk = await checkCardConservation(gameId);
            assert.ok(chk.ok, `card conservation violated at step ${step}: ${chk.detail}`);
        }
        const evts = broadcastLog.filter((b) => b.event === 'animation_events');
        assert.ok(evts.length > 0, 'expected broadcasts');
        const perChannel = new Map<string, number[]>();
        for (const e of evts) {
            assert.equal(typeof e.payload.v, 'number', 'packed broadcast carries a numeric version (payload.v)');
            if (!perChannel.has(e.channel)) perChannel.set(e.channel, []);
            perChannel.get(e.channel)!.push(e.payload.v);
        }
        for (const [chan, vs] of perChannel)
            for (let i = 1; i < vs.length; i++) assert.ok(vs[i] > vs[i - 1], `versions not increasing on ${chan}: ${vs.join(',')}`);
    });

    test('server: a burst of overlapping submits against one version cannot duplicate or lose a card', async () => {
        const { gameId, botIds } = await newGame(3, 1);
        for (let step = 0; step < 25; step++) {
            const snap = await loadGame(gameId);
            if (snap.status !== 'playing') break;
            const moves = legalMovesFor(snap, (id) => !botIds.has(id));
            if (moves.length === 0) continue;
            const burst: PlayerMove[] = [pick(moves), pick(moves)];
            burst.push(burst[0]); // rapid double-submit
            await Promise.all(burst.map((m, i) => executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, m) }), `b${step}-${i}`, true).catch(() => {})));
            const chk = await checkCardConservation(gameId);
            assert.ok(chk.ok, `conservation broke under contention at step ${step}: ${chk.detail}`);
        }
    });
}

if (!process.env.VALIDATION_ONLY) {
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
    // Each animation_events broadcast must carry a numeric version (the packed
    // payload's `v`); per recipient channel, versions must be strictly
    // increasing in emission order.
    const evts = broadcastLog.filter((b) => b.event === 'animation_events');
    assert.ok(evts.length > 0, 'expected broadcasts');
    const perChannel = new Map<string, number[]>();
    for (const e of evts) {
        assert.equal(typeof e.payload.v, 'number', 'broadcast payload must carry a version (payload.v)');
        if (!perChannel.has(e.channel)) perChannel.set(e.channel, []);
        perChannel.get(e.channel)!.push(e.payload.v);
    }
    for (const [chan, versions] of perChannel) {
        for (let i = 1; i < versions.length; i++) {
            assert.ok(versions[i] > versions[i - 1], `versions not strictly increasing on ${chan}: ${versions.join(',')}`);
        }
    }
});

test('every finished game gets a replay snapshot and its logs wiped (log order is deterministic)', async () => {
    // Regression check for the replay-desync glitch: a move's cascade stamps
    // several logs in the same millisecond, and ordering the session by
    // created_at alone returned those ties in arbitrary order — the encoder then
    // threw "replay desync: logged attack not in menu" and finished games kept
    // raw logs instead of a snapshot (~1 in 5 games). The fix orders by
    // (created_at, seq). Several games make a regression's tie-scramble likely.
    let finished = 0;
    for (let round = 0; round < 6; round++) {
        await resetDb();
        const { gameId } = await newGame(1, 2);
        let steps = 0;
        while (steps < 600) {
            const g = await loadGame(gameId);
            if (g.status !== 'playing') break;
            const moves = legalMovesFor(g);
            if (moves.length === 0) break;
            try { await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, pick(moves)) }), `s${steps}`, true); } catch { /* stale */ }
            steps++;
        }
        const g = await loadGame(gameId);
        if (g.status !== 'game_over') continue;
        finished++;
        const snaps = await pgPool.query('SELECT moves FROM game_snapshots WHERE game_id=$1', [gameId]);
        assert.equal(snaps.rowCount, 1, `finished game ${gameId} has no replay snapshot — the encoder desynced (check log ordering)`);
        const logs = await pgPool.query('SELECT COUNT(*)::int AS n FROM game_logs WHERE game_id=$1', [gameId]);
        assert.equal(logs.rows[0].n, 0, `finished game ${gameId} kept ${logs.rows[0].n} raw logs — snapshot should have replaced them`);
        // The packed session-log column (the snapshot's actual source since
        // the logwire cutover) is retired the same way after a snapshot.
        const packed = await pgPool.query('SELECT logs_packed FROM games WHERE id=$1', [gameId]);
        assert.equal(packed.rows[0].logs_packed, '', `finished game ${gameId} kept its packed session log — snapshot should have retired it`);
    }
    assert.ok(finished >= 2, `expected at least 2 finished games, got ${finished}`);
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

registerServerValidation();

after(async () => { await pgPool.end(); });
}
