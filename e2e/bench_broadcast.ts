// Microbench: wall-clock of ONE broadcastAnimationEvents call, driving the REAL
// shipped function against the real adapter transport. To make the comparison
// meaningful (the in-process shim has ~0 network cost), inject a per-POST latency
// via E2E_BCAST_LATENCY_MS — applied identically to the old channel.send() shim
// and the new batched-fetch shim. Run this script on `main` (old N+1 sends) and
// on the branch (1 batched send) with the same env to compare.
//
//   E2E_BCAST_LATENCY_MS=60 BENCH_HUMANS=6 BENCH_ITERS=20 \
//     node --import tsx e2e/bench_broadcast.ts
import './harness.ts';
import { applySchema, resetDb, seedGame, uuid, pgPool, broadcastLog, resetBroadcastLog } from './harness.ts';
import { executeWithGameLock, broadcastAnimationEvents, loadCompleteGame } from '../supabase/functions/_shared/utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import { AnimationEvent } from '../supabase/functions/_shared/types.ts';

const HUMANS = Number(process.env.BENCH_HUMANS || 6);
const BOTS = Number(process.env.BENCH_BOTS || 0);
const ITERS = Number(process.env.BENCH_ITERS || 20);
const LAT = Number(process.env.E2E_BCAST_LATENCY_MS || 0);

async function main() {
    await applySchema();
    await resetDb();

    const gameId = `g${uuid().slice(0, 6)}`;
    const players = [
        ...Array.from({ length: HUMANS }, (_, i) => ({ id: uuid(), name: `H${i}`, is_ai: false, strategy_key: 'human' })),
        ...Array.from({ length: BOTS }, (_, i) => ({ id: uuid(), name: `B${i}`, is_ai: true, strategy_key: 'random' })),
    ];
    await seedGame(gameId, players);

    // Capture a real game + real events (the dealt-hands start sequence).
    let events: AnimationEvent[] = [];
    await executeWithGameLock(gameId, async (g) => { events = start_game(g) as AnimationEvent[]; return { game: g, events }; }, 'start', false);
    const game = await loadCompleteGame(gameId);

    // Warm up (JIT, connections), not measured.
    await broadcastAnimationEvents(game, events, 'warm');
    resetBroadcastLog();

    const samples: number[] = [];
    for (let i = 0; i < ITERS; i++) {
        const t0 = performance.now();
        await broadcastAnimationEvents(game, events, `bench${i}`);
        samples.push(performance.now() - t0);
    }

    samples.sort((a, b) => a - b);
    const mean = samples.reduce((a, b) => a + b, 0) / samples.length;
    const median = samples[Math.floor(samples.length / 2)];
    const posts = broadcastLog.length / ITERS; // recorded messages per broadcast

    console.error(`\n=== broadcast bench ===`);
    console.error(`humans=${HUMANS}  bots=${BOTS}  iters=${ITERS}  injected per-POST latency=${LAT}ms`);
    console.error(`messages recorded per broadcast: ${posts}  (expected ${HUMANS} players + 1 spectator = ${HUMANS + 1}; bots get nothing)`);
    console.error(`wall-clock per broadcast:  mean=${mean.toFixed(1)}ms  median=${median.toFixed(1)}ms  min=${samples[0].toFixed(1)}ms  max=${samples[samples.length - 1].toFixed(1)}ms`);

    await pgPool.end();
}

main().catch((e) => { console.error(e); process.exit(1); });
