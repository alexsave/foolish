// E2E: drives the REAL deployed server orchestration - the table_io CAS loop,
// the C Table's move and deal, commit_table, the bot lease, the broadcast -
// against a real Postgres. The only substitution is PostgREST/Realtime,
// replaced by the small pg adapter. Every seat, bots included, moves through
// the move path here (the bot loop has its own suites), so the suite seed
// reproduces every game it plays.

import './harness.ts'; // sets Deno globals BEFORE any server module loads
import { test, before, after, beforeEach } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, uuid, pgPool, broadcastLog } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { checkCardConservation, legalMoves, mustReadTable, type PlayMove } from './helpers/table_play.ts';
import { runAction, runMeta, seedLobby } from './helpers/table_server.ts';
import { suiteRng } from './helpers/rng.ts';

// One stream for the whole file: every test here draws sequentially, so the
// suite seed reproduces the exact move sequence of every game it plays.
const rng = suiteRng('server');
const rand = (n: number) => rng.int(n);
const pick = <T>(a: T[]): T => rng.pick(a);

const playing = (t: { status: number }) => t.status === L.GAME_STATUS_PLAYING;
const move = (gameId: string, m: PlayMove) => runAction(gameId, m.playerId, m).catch(() => { /* a stale pick under the CAS */ });

async function newGame(humans: number, bots: number): Promise<{ gameId: string; humanIds: string[]; botIds: Set<string> }> {
    const gameId = `g${uuid().slice(0, 6)}`;
    const humanIds: string[] = [];
    const botIds = new Set<string>();
    const seats = [];
    for (let i = 0; i < humans; i++) { const id = uuid(); humanIds.push(id); seats.push({ id, name: `H${i}`, ready: i > 0 }); }
    for (let i = 0; i < bots; i++) { const id = uuid(); botIds.add(id); seats.push({ id, name: `B${i}`, brain: 'random' }); }
    await seedLobby(gameId, seats);
    await runMeta(gameId, humanIds[0], { type: 'start' });
    assert.ok(playing(await mustReadTable(gameId)), 'fixture: the game dealt');
    return { gameId, humanIds, botIds };
}

// ---- handpicked validation: a short game + a burst, sharing DB before/after ---
export function registerServerValidation(): void {
    test('server: a short real game conserves cards and broadcasts strictly-increasing versions', async () => {
        const { gameId } = await newGame(2, 1);
        for (let step = 0; step < 40; step++) {
            const t = await mustReadTable(gameId);
            if (!playing(t)) break;
            const moves = legalMoves(t);
            if (moves.length === 0) break;
            await move(gameId, pick(moves));
            const chk = await checkCardConservation(gameId);
            assert.ok(chk.ok, `card conservation violated at step ${step} (seed=${rng.seed}): ${chk.detail}`);
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
            const snap = await mustReadTable(gameId);
            if (!playing(snap)) break;
            const moves = legalMoves(snap, (s) => !botIds.has(s.id));
            if (moves.length === 0) continue;
            const burst: PlayMove[] = [pick(moves), pick(moves)];
            burst.push(burst[0]); // rapid double-submit
            await Promise.all(burst.map((m) => move(gameId, m)));
            const chk = await checkCardConservation(gameId);
            assert.ok(chk.ok, `conservation broke under contention at step ${step} (seed=${rng.seed}): ${chk.detail}`);
        }
    });
}

if (!process.env.VALIDATION_ONLY) {
before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });

test('card conservation holds across a full sequential game (real table_io CAS loop + C Table + commit_table)', async () => {
    const { gameId } = await newGame(2, 1);
    let steps = 0;
    while (steps < 3000) {
        const t = await mustReadTable(gameId);
        if (!playing(t)) break;
        const moves = legalMoves(t);
        if (moves.length === 0) break;
        await move(gameId, pick(moves));
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `card conservation violated at step ${steps} (seed=${rng.seed}): ${chk.detail}`);
        steps++;
    }
    assert.ok(steps > 5, `game should have progressed (seed=${rng.seed})`);
});

test('every broadcast carries a monotonically non-decreasing games.version (the reordering fix, server side)', async () => {
    const { gameId } = await newGame(2, 1);
    let steps = 0;
    while (steps < 2000) {
        const t = await mustReadTable(gameId);
        if (!playing(t)) break;
        const moves = legalMoves(t);
        if (moves.length === 0) break;
        await move(gameId, pick(moves));
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
    // several logs in the same millisecond, and a session whose records lost
    // their order made the encoder throw and left finished games with raw logs
    // instead of a snapshot (~1 in 5 games). Several games make a regression
    // likely to show.
    let finished = 0;
    for (let round = 0; round < 6; round++) {
        await resetDb();
        const { gameId } = await newGame(1, 2);
        let steps = 0;
        while (steps < 600) {
            const t = await mustReadTable(gameId);
            if (!playing(t)) break;
            const moves = legalMoves(t);
            if (moves.length === 0) break;
            await move(gameId, pick(moves));
            steps++;
        }
        const t = await mustReadTable(gameId);
        if (t.statusColumn !== 'game_over') continue;
        finished++;
        const snaps = await pgPool.query('SELECT moves FROM game_snapshots WHERE game_id=$1', [gameId]);
        assert.equal(snaps.rowCount, 1, `finished game ${gameId} has no replay snapshot (seed=${rng.seed}) - the encoder desynced (check log ordering)`);
        // The packed session-log column (the snapshot's source) is retired after
        // a snapshot — game_logs rows are gone (migration 20260708120000).
        const packed = await pgPool.query('SELECT logs_packed FROM games WHERE id=$1', [gameId]);
        assert.equal(packed.rows[0].logs_packed, '', `finished game ${gameId} kept its packed session log (seed=${rng.seed}) - snapshot should have retired it`);
    }
    assert.ok(finished >= 2, `expected at least 2 finished games, got ${finished} (seed=${rng.seed})`);
});

test('CAS serializes concurrent moves without losing or duplicating a card', async () => {
    const { gameId, botIds } = await newGame(3, 1);
    let steps = 0;
    while (steps < 1500) {
        const snap = await mustReadTable(gameId);
        if (!playing(snap)) break;
        const moves = legalMoves(snap, (s) => !botIds.has(s.id));
        if (moves.length === 0) { steps++; continue; }
        // fire a burst of overlapping requests against the same loaded version
        const burst: PlayMove[] = [];
        const n = 1 + rand(Math.min(3, moves.length));
        for (let i = 0; i < n; i++) burst.push(pick(moves));
        if (rng.chance(0.3)) burst.push(burst[0]); // rapid double-submit
        await Promise.all(burst.map((m) => move(gameId, m)));
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `conservation broke under contention at step ${steps} (seed=${rng.seed}): ${chk.detail}`);
        steps++;
    }
});

registerServerValidation();

after(async () => { await pgPool.end(); });
}
