// END-TO-END latency of a THINKING bot's turn, against REAL Postgres - the
// number a human actually waits on and the one the 2s CPU cap gates. Every
// other metric (wasm size -> module load, engine speed, wasm memory -> MC world
// allocation) feeds into THIS.
//
// bench_e2e_move.ts times the full server path for cheap HUMAN moves. This times
// one bot-loop cycle (bot_actions.ts) driven by the belief/Monte-Carlo bots -
// octogen/cordite/blackpowder/firecracker - where the kernel deliberation
// dominates. Per decision it brackets the whole production pipeline: the row
// read, the C Table load, the session-log read when the kernel says a belief bot
// is about to choose, the drive (choose + apply), the commit products and the
// commit_table CAS commit. The only thing left out is the deliberate inter-bot UX
// pacing (a sleep, not latency).
//
//   BENCH_BOTS=octogen,cordite BENCH_BOT_MOVES=25 \
//     E2E_PGUSER=stress E2E_PGPASSWORD=stress E2E_PGDATABASE=foolish \
//     TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/bench_bot_e2e.ts

import './harness.ts';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { __botsWasmMB, __botsWasmBytes, kernelBotRoster } from '../sdk/ts/wasm/bots.ts';
import { __botCycle } from '../server/impls/supabase/functions/_shared/adapter/bot_actions.ts';
import { __setTableDealSeedOverride } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { mustReadTable } from './helpers/table_play.ts';
import { runMeta, seedLobby } from './helpers/table_server.ts';

const say = (l: string) => process.stdout.write(l + '\n'); // harness silences console.log
const JSON_OUT = process.env.BENCH_JSON === '1';
// Shipped bots only. An unknown key is refused rather than defaulted away: the
// C Table refuses a brain this build does not link (TABLE_E_UNKNOWN_BRAIN), and
// a benchmark that silently measured a different bot would be worse than none.
const BOTS = (process.env.BENCH_BOTS || 'octogen,cordite,blackpowder,firecracker').split(',').map(s => s.trim()).filter(Boolean);
const MOVES = Number(process.env.BENCH_BOT_MOVES || 25);

const nowMs = () => Number(process.hrtime.bigint()) / 1e6;

async function benchStrategy(strategy: string): Promise<{ strategy: string; n: number; mean: number; p50: number; p90: number; p95: number; max: number; beliefHydrated: boolean }> {
    await resetDb();
    const gameId = `be${uuid().slice(0, 5)}`;
    // Two bots of the brain, seated READY; a ready by a seat deals the table.
    const b0 = uuid();
    await seedLobby(gameId, [{ id: b0, name: 'B0', brain: strategy }, { id: uuid(), name: 'B1', brain: strategy }]);
    __setTableDealSeedOverride(Uint8Array.from({ length: 32 }, (_, i) => (i * 37 + 0x0c) & 0xff));
    await runMeta(gameId, b0, { type: 'start' });
    const samples: number[] = [];
    let beliefHydrated = false;
    let guard = 0;
    while (samples.length < MOVES && ++guard < MOVES * 6) {
        const before = await mustReadTable(gameId);
        if (before.status !== L.GAME_STATUS_PLAYING) break;
        if (!before.needsBotsColumn) break;
        const t0 = nowMs();
        await __botCycle(gameId);
        const dt = nowMs() - t0;
        const after = await mustReadTable(gameId);
        if (after.logsPacked) beliefHydrated = true;
        // Only time real committed decisions; a cycle with nothing to drive (the
        // human owes a move) is not a decision.
        if (after.version !== before.version) samples.push(dt);
        else break;
    }

    samples.sort((a, b) => a - b);
    const mean = samples.reduce((x, y) => x + y, 0) / (samples.length || 1);
    const pick = (q: number) => samples[Math.min(samples.length - 1, Math.floor(q * samples.length))] ?? 0;
    return { strategy, n: samples.length, mean, p50: pick(0.5), p90: pick(0.9), p95: pick(0.95), max: samples[samples.length - 1] ?? 0, beliefHydrated };
}

async function main() {
    const known = new Set(kernelBotRoster().map((e) => e.key));
    const unknown = BOTS.filter((k) => !known.has(k));
    if (unknown.length > 0) {
        throw new Error(`BENCH_BOTS names ${unknown.join(', ')}, which this bots.wasm does not link. Known keys: ${[...known].join(', ')}`);
    }
    await applySchema();
    const results = [];
    for (const strat of BOTS) results.push(await benchStrategy(strat));

    // Peak wasm linear memory after the MC bots ran (the external-budget number).
    // One module serves the server now (bots.wasm is the kernel): the kernel
    // columns the metrics renderer reads are the same instance.
    const memory = {
        botsWasmMB: __botsWasmMB(), kernelWasmMB: __botsWasmMB(),
        botsWasmBytes: __botsWasmBytes(), kernelWasmBytes: __botsWasmBytes(),
    };

    if (JSON_OUT) { say(JSON.stringify({ e2e: results, memory })); await pgPool.end(); return; }
    say(`thinking-bot E2E latency vs real Postgres (load -> table -> session log -> drive -> commit), ${MOVES} decisions/bot`);
    say(`belief hydrated: ${results.some(r => r.beliefHydrated) ? 'yes' : 'no'}`);
    for (const r of results) {
        say(`  ${r.strategy.padEnd(10)} n=${String(r.n).padStart(3)}  mean ${r.mean.toFixed(1).padStart(6)}ms   p50 ${r.p50.toFixed(1).padStart(6)}ms   p90 ${r.p90.toFixed(1).padStart(6)}ms   p95 ${r.p95.toFixed(1).padStart(6)}ms   max ${r.max.toFixed(0)}ms`);
    }
    say(`wasm memory: bots=${memory.botsWasmMB}MB`);
    await pgPool.end();
}

main().catch((e) => { console.error(e); process.exit(1); });
