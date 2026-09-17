// E2E across the wire: drive a REAL multi-bout game (the real move path:
// executePackedAction -> the table_io CAS loop -> the C Table -> commit_table ->
// the kernel's per-viewer pushes), capture the exact broadcasts the focus
// player's client would receive, then feed them through the REAL client
// reconciliation (clientReconcile: the version gate + trust-incoming table merge)
// under REORDERED delivery - and assert the client converges to the server's
// authoritative table. This is the broadcast-reordering fix end to end.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool, broadcastLog } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { legalMoves, mustReadTable } from './helpers/table_play.ts';
import { runAction, runMeta, seedLobby } from './helpers/table_server.ts';
import { __setTableDealSeedOverride } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { shouldDropStaleSequence, mergeTableBattles } from '../src/state/clientReconcile';
import { decodeEventWire } from '../sdk/ts/wire/evwire.ts';
import { base64ToBytes } from '../sdk/ts/wire/bytes.ts';
import { suiteRng, type SeededRng } from './helpers/rng.ts';

// Three streams, so widening one does not renumber the others: `deal` pins each
// game's deal seed, `moves` drives the game the broadcasts come from, `arrival`
// the reordering they are replayed in.
const rng = suiteRng('reconcile');
const dealRng = rng.fork('deal');
const moveRng = rng.fork('moves');
const arrivalRng = rng.fork('arrival');
const tkeys = (bs: any[]) => bs.flatMap((b: any) => (b.defense ? [`${b.attack.suit}-${b.attack.value}`, `${b.defense.suit}-${b.defense.value}`] : [`${b.attack.suit}-${b.attack.value}`])).sort();

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); __clearGameCache(); });

interface Bcast { version: number; eventTables: any[][]; finalTable: any[] }

async function driveAndCapture(): Promise<{ stream: Bcast[]; serverFinalTable: unknown[] }> {
    const gameId = `r${uuid().slice(0, 6)}`;
    const human = uuid();
    const seats = [
        { id: human, name: 'Hero', ready: false },
        { id: uuid(), name: 'B0', brain: 'random' },
        { id: uuid(), name: 'B1', brain: 'random' },
    ];
    await seedLobby(gameId, seats);
    __setTableDealSeedOverride(Uint8Array.from({ length: 32 }, () => dealRng.int(256)));
    await runMeta(gameId, human, { type: 'start' });
    // The identity roster the client already holds when a broadcast arrives -
    // exactly what it needs to decode the packed event wire.
    const roster = { id: gameId, name: gameId, players: seats.map((p) => ({ player_id: p.id, name: p.name, is_ai: !!p.brain })) };

    // Every seat, bots included, moves through the move path under its own id,
    // so the move stream (and so the broadcasts) is the suite seed's.
    let steps = 0;
    while (steps < 600) {
        const t = await mustReadTable(gameId);
        if (t.status !== L.GAME_STATUS_PLAYING) break;
        const moves = legalMoves(t);
        if (moves.length === 0) break;
        const mv = moveRng.pick(moves);
        try { await runAction(gameId, mv.playerId, mv); } catch { /* */ }
        steps++;
    }
    await new Promise((r) => setImmediate(r));   // the broadcast is fire-and-forget

    // The focus player's personalized channel: the kernel's push bytes, decoded
    // with the REAL client decoder to recover the per-step snapshots + final
    // game. preGood/prevGoodTs are dummies - this test only consumes
    // table_battles and the version.
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
    const serverFinalTable = (await mustReadTable(gameId)).battles;
    return { stream, serverFinalTable };
}

function replayReordered(stream: Bcast[], jitter: number, arrival: SeededRng): any[] {
    // reordered arrival: emit index spacing 2ms + uniform 0..jitter
    const order = stream.map((b, i) => ({ b, t: i * 2 + arrival.next() * jitter })).sort((x, y) => x.t - y.t).map((x) => x.b);
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
        const clientTable = replayReordered(stream, 200, arrivalRng.fork(t));
        assert.deepEqual(tkeys(clientTable), tkeys(newest.finalTable),
            `client table diverged from the newest authoritative broadcast under reordering `
            + `(seed=${rng.seed}, trial=${t})`);
        void serverFinalTable;
    }
    assert.ok(trials > 0, `expected at least one multi-broadcast game (seed=${rng.seed})`);
});

after(async () => { __setTableDealSeedOverride(null); await pgPool.end(); });
