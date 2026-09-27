// Microbench: wall-clock of ONE broadcastPushes call (table_io.ts), driving the
// REAL shipped function against the real adapter transport. The pushes are a
// deal's (the fattest broadcast in a game), written by the C Table. To make a
// comparison meaningful (the in-process shim has ~0 network cost), inject a
// per-POST latency via E2E_BCAST_LATENCY_MS.
//
//   E2E_BCAST_LATENCY_MS=60 BENCH_HUMANS=6 BENCH_ITERS=20 \
//     TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/bench_broadcast.ts
import './harness.ts';
import { applySchema, uuid, pgPool, broadcastLog, resetBroadcastLog } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { broadcastPushes } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { fixture, fixtureTable, READY } from './helpers/table_fixture.ts';

const HUMANS = Number(process.env.BENCH_HUMANS || 6);
const BOTS = Number(process.env.BENCH_BOTS || 0);
const ITERS = Number(process.env.BENCH_ITERS || 20);
const LAT = Number(process.env.E2E_BCAST_LATENCY_MS || 0);

async function main() {
    await applySchema();

    // A real deal's pushes: every seat ready but one, which readies and deals.
    const gameId = `g${uuid().slice(0, 6)}`;
    const seats = [
        ...Array.from({ length: HUMANS }, (_, i) => ({ id: uuid(), name: `H${i}` })),
        ...Array.from({ length: BOTS }, (_, i) => ({ id: uuid(), name: `B${i}`, brain: 'random' })),
    ];
    let b = fixture().seats(seats);
    seats.forEach((_, i) => { if (i > 0) b = b.seatStatus(i, READY); });
    const fx = b.build();
    const table = fixtureTable();
    if (table.load(fx.state, fx.roster) !== L.TABLE_OK) throw new Error('the lobby does not load');
    if (table.ready(seats[0].id, new Uint8Array(32)) !== L.TABLE_OK) throw new Error('the deal was refused');
    const p = table.commit(gameId, 1, 0);
    if (typeof p === 'number') throw new Error(`no products (${p})`);
    const roster = table.seats();
    const pushes: { viewer: number; bytes: Uint8Array }[] = [];
    for (const viewer of [...roster.flatMap((s, i) => (s.brain ? [] : [i])), -1]) {
        const push = table.push(gameId, viewer);
        if (typeof push === 'number') throw new Error(`push refused (${push})`);
        pushes.push({ viewer, bytes: push });
    }

    // Warm up (JIT, connections), not measured.
    await broadcastPushes(gameId, 1, roster, pushes, 'warm');
    resetBroadcastLog();

    const samples: number[] = [];
    for (let i = 0; i < ITERS; i++) {
        const t0 = performance.now();
        await broadcastPushes(gameId, 1, roster, pushes, `bench${i}`);
        samples.push(performance.now() - t0);
    }

    samples.sort((x, y) => x - y);
    const mean = samples.reduce((x, y) => x + y, 0) / samples.length;
    const median = samples[Math.floor(samples.length / 2)];
    const posts = broadcastLog.length / ITERS; // recorded messages per broadcast

    console.error(`\n=== broadcast bench ===`);
    console.error(`humans=${HUMANS}  bots=${BOTS}  iters=${ITERS}  injected per-POST latency=${LAT}ms`);
    console.error(`messages recorded per broadcast: ${posts}  (expected ${HUMANS} players + 1 spectator = ${HUMANS + 1}; bots get nothing)`);
    console.error(`wall-clock per broadcast:  mean=${mean.toFixed(1)}ms  median=${median.toFixed(1)}ms  min=${samples[0].toFixed(1)}ms  max=${samples[samples.length - 1].toFixed(1)}ms`);

    await pgPool.end();
}

main().catch((e) => { console.error(e); process.exit(1); });
