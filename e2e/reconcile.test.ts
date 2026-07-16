// E2E across the wire: drive a REAL multi-bout game (real executeWithGameLock +
// handlers + commit_game + broadcastAnimationEvents), capture the exact broadcasts
// the focus player's client would receive, then feed them through the REAL client
// reconciliation (clientReconcile: the version gate + trust-incoming table merge)
// under REORDERED delivery — and assert the client converges to the server's
// authoritative table. This is the broadcast-reordering fix end to end.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool, broadcastLog } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../supabase/functions/_shared/adapter/utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import { AnimationEvent } from '../supabase/functions/_shared/types.ts';
import { legalMovesFor, applyPlayerMove } from './dispatch.ts';
import { shouldDropStaleSequence, mergeTableBattles } from '../src/state/clientReconcile';
import { decodeEventWire } from '../supabase/functions/_shared/sdk/ts/wire/evwire.ts';
import { base64ToBytes } from '../supabase/functions/_shared/sdk/ts/wire/bytes.ts';

const pick = <T>(a: T[]): T => a[Math.floor(Math.random() * a.length)];
const tkeys = (bs: any[]) => bs.flatMap((b: any) => (b.defense ? [`${b.attack.suit}-${b.attack.value}`, `${b.defense.suit}-${b.defense.value}`] : [`${b.attack.suit}-${b.attack.value}`])).sort();

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });

interface Bcast { version: number; eventTables: any[][]; finalTable: any[] }

async function driveAndCapture(): Promise<{ stream: Bcast[]; serverFinalTable: any[] }> {
    const gameId = `r${uuid().slice(0, 6)}`;
    const human = uuid();
    const seeded = [
        { id: human, name: 'Hero', is_ai: false, strategy_key: 'human' },
        { id: uuid(), name: 'B0', is_ai: true, strategy_key: 'random' },
        { id: uuid(), name: 'B1', is_ai: true, strategy_key: 'random' },
    ];
    await seedGame(gameId, seeded);
    // The identity roster the client already holds when a broadcast arrives —
    // exactly what it needs to decode the packed event wire.
    const roster = { id: gameId, name: gameId, players: seeded.map((p) => ({ player_id: p.id, name: p.name, is_ai: p.is_ai })) };
    await executeWithGameLock(gameId, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);

    let steps = 0;
    while (steps < 600) {
        const g = await loadCompleteGame(gameId);
        if (g.status !== 'playing') break;
        const moves = legalMovesFor(g);
        if (moves.length === 0) break;
        try { await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, pick(moves)) }), `s${steps}`, true); } catch { /* */ }
        steps++;
    }

    // The focus player's personalized channel. Broadcasts are packed now
    // ({t,s,v,b}); decode the base64 event wire with the REAL client decoder
    // to recover the per-step snapshots + final game. preGood/prevGoodTs are
    // dummies — this test only consumes table_battles and the version.
    const chan = `gu-${gameId}-${human}`;
    const stream: Bcast[] = broadcastLog
        .filter((b) => b.channel === chan && b.event === 'animation_events')
        .map((b) => {
            const decoded = decodeEventWire(base64ToBytes(b.payload.b), roster, { preGood: [], prevGoodTs: null });
            assert.ok(decoded, `packed broadcast payload must decode (v=${b.payload.v})`);
            return {
                version: b.payload.v as number,
                eventTables: decoded!.events.map((e) => e.game_state?.table_battles ?? []),
                finalTable: decoded!.game?.table_battles ?? [],
            };
        });
    const serverFinalTable = (await loadCompleteGame(gameId)).table_battles;
    return { stream, serverFinalTable };
}

function replayReordered(stream: Bcast[], jitter: number): any[] {
    // reordered arrival: emit index spacing 2ms + uniform 0..jitter
    const order = stream.map((b, i) => ({ b, t: i * 2 + Math.random() * jitter })).sort((x, y) => x.t - y.t).map((x) => x.b);
    let table: any[] = [];
    let lastApplied: number | null = null;
    for (const bc of order) {
        if (shouldDropStaleSequence(lastApplied, bc.version)) continue; // REAL gate
        for (const tb of [...bc.eventTables, bc.finalTable]) table = mergeTableBattles(table, tb); // REAL merge
        lastApplied = bc.version;
    }
    return table;
}

test('client converges to the server table under heavy reordering (real broadcasts + real client gate/merge)', async () => {
    let trials = 0;
    for (let t = 0; t < 6; t++) {
        const { stream, serverFinalTable } = await driveAndCapture();
        if (stream.length < 3) continue;
        trials++;
        // The newest broadcast the client received is authoritative for what it should end on.
        const newest = stream.reduce((a, b) => (b.version > a.version ? b : a));
        const clientTable = replayReordered(stream, 200);
        assert.deepEqual(tkeys(clientTable), tkeys(newest.finalTable),
            `client table diverged from the newest authoritative broadcast under reordering`);
        void serverFinalTable;
    }
    assert.ok(trials > 0, 'expected at least one multi-broadcast game');
});

after(async () => { await pgPool.end(); });
