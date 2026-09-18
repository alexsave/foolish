// End-to-end per-move wall clock against REAL Postgres: the full server path
// minus HTTP/auth - row load (or this isolate's row cache), the C Table's
// load / act / commit products / pushes, the commit_table CAS commit, and the
// broadcast (shimmed local). This is executePackedAction, what the binary
// `action` path runs.
//
//   E2E_DB_PREFIX=c03m BENCH_E2E_MOVES=300 TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/bench_e2e_move.ts
//
// The "packed" line is the gate in docs/C_GAME_SHAPE_MIGRATION.md 4.0 (action
// latency, human move). E2E_DB_RTT_MS=<ms> adds that round trip to every
// database request (the hosted-like variant); the commit_table line is the JSON
// body PostgREST would receive per commit. Before Phase 4b the same line measured the TS pipeline
// (runPackedAction + commit_game); the label is kept so the numbers line up.
import './harness.ts';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import { rpcBodyBytes } from './adapters/supabase.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { legalMoves, mustReadTable } from './helpers/table_play.ts';
import { runAction, runMeta, seedLobby } from './helpers/table_server.ts';
import { __setTableDealSeedOverride } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';

const MOVES = Number(process.env.BENCH_E2E_MOVES || 300);
const say = (line: string) => process.stdout.write(line + '\n'); // harness silences console.log
let seed = 0xabcd;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
const ri = (n: number) => Math.floor(rnd() * n);

async function freshGame(k: number): Promise<string> {
    const gameId = `b${uuid().slice(0, 5)}`;
    const pids = [uuid(), uuid(), uuid(), uuid()];
    await seedLobby(gameId, pids.map((id, i) => ({ id, name: `P${i}`, ready: i > 0 })));
    __setTableDealSeedOverride(Uint8Array.from({ length: 32 }, (_, i) => (i * 29 + k * 7 + 3) & 0xff));
    await runMeta(gameId, pids[0], { type: 'start' });
    return gameId;
}

async function run(): Promise<{ moves: number; ns: bigint; samples: number[] }> {
    let moves = 0; let ns = 0n; let games = 0;
    const samples: number[] = []; // per-move ms, for the percentiles
    while (moves < MOVES) {
        const gameId = await freshGame(games++);
        for (let mv = 0; mv < 400 && moves < MOVES; mv++) {
            // Pick the move OUTSIDE the timer (a client does this locally).
            const t = await mustReadTable(gameId);
            if (t.status !== L.GAME_STATUS_PLAYING) break;
            const menu = legalMoves(t);
            if (menu.length === 0) break;
            const m = menu[ri(menu.length)];

            const t0 = process.hrtime.bigint();
            try {
                await runAction(gameId, m.playerId, m);
                const dt = process.hrtime.bigint() - t0;
                ns += dt;
                samples.push(Number(dt) / 1e6);
                moves++;
            } catch { /* rare edge - skip uncounted */ }
        }
    }
    return { moves, ns, samples };
}

(async () => {
    await applySchema();
    say(`end-to-end move bench vs real Postgres: 4 humans, ${MOVES} moves (load+C Table+CAS commit+shimmed broadcast)`);
    await resetDb();
    seed = 0xabcd;
    await run(); // warm pass
    await resetDb();
    seed = 0xabcd;
    rpcBodyBytes.clear();
    const { moves, ns, samples } = await run();
    const us = Number(ns) / 1000 / moves;
    samples.sort((a, b) => a - b);
    const pick = (q: number) => samples[Math.min(samples.length - 1, Math.floor(q * samples.length))] ?? 0;
    say(`  packed ${(us / 1000).toFixed(2).padStart(7)} ms/move mean   p50 ${pick(0.5).toFixed(2)} ms   p95 ${pick(0.95).toFixed(2)} ms   (${moves} moves, rtt ${process.env.E2E_DB_RTT_MS || 0} ms)`);
    const commits = rpcBodyBytes.get('commit_table');
    if (commits) say(`  commit_table body ${Math.round(commits.bytes / commits.calls)} B/commit mean over ${commits.calls} commits (the deals included)`);
    await pgPool.end();
})();
