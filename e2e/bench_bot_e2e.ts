// END-TO-END latency of a THINKING bot's turn, against REAL Postgres — the
// number a human actually waits on and the one the 2s CPU cap gates. Every
// other metric (wasm size → module load, engine speed → marshal/enumerate,
// wasm memory → MC world allocation) feeds into THIS.
//
// bench_e2e_move.ts already times the full server path (load → kernel → CAS
// commit) but only for cheap HUMAN moves. This times the same path driven by
// the belief/Monte-Carlo bots — octogen/semtex/cordite/fulminate — where the
// kernel deliberation dominates. Per decision it brackets the whole production
// pipeline: loadCompleteGame (real row read) → belief hydrate (if wired) →
// kernel choose (marshal + belief + MC) → apply → CAS commit_game. The only
// thing left out is the deliberate inter-bot UX pacing (a sleep, not latency).
//
//   BENCH_BOTS=octogen,cordite BENCH_BOT_MOVES=25 \
//     E2E_PGUSER=stress E2E_PGPASSWORD=stress E2E_PGDATABASE=foolish \
//     TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/bench_bot_e2e.ts

import './harness.ts';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock } from '../supabase/functions/_shared/utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/bot_strategy.ts';
import { processBotActionPacked, shouldBotActCore } from '../supabase/functions/_shared/pure_bot_actions.ts';
import { __setBotSeedSource, __botsWasmMB } from '../supabase/functions/_shared/wasm/bots.ts';
import { __setKernelSeedSource, __kernelWasmMB } from '../supabase/functions/_shared/wasm/engine.ts';
import { bytesToHex } from '../supabase/functions/_shared/replay/codec.ts';
import { bytesToBareHex } from '../supabase/functions/_shared/wire/bytes.ts';
import { logsFromKernelExport } from '../supabase/functions/_shared/wire/logwire.ts';
import { AnimationEvent, Game, GAME_STATUS, PLAYER_STATUS } from '../supabase/functions/_shared/types.ts';

const say = (l: string) => process.stdout.write(l + '\n'); // harness silences console.log
const JSON_OUT = process.env.BENCH_JSON === '1';
const BOTS = (process.env.BENCH_BOTS || 'octogen,semtex,cordite,fulminate').split(',').map(s => s.trim()).filter(Boolean);
const MOVES = Number(process.env.BENCH_BOT_MOVES || 25);

const mkLcgU32 = (seed: number) => { let s = (seed >>> 0) || 1; return () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s; }; };
const nowMs = () => Number(process.hrtime.bigint()) / 1e6;

// Belief hydration is feature-detected so this bench measures the TRUE pipeline
// on either side of the belief-log fix: on branches that carry loadSessionLogs
// the belief bots see the session (the intended behavior); on older branches
// they don't (and importLogs just sees an empty log). Same code path the server
// bot loop runs. Resolved in main() (no top-level await under tsx's CJS output).
let loadSessionLogs: ((id: string, players: { player_id: string }[]) => Promise<unknown[]>) | null = null;

async function benchStrategy(strategy: string): Promise<{ strategy: string; n: number; mean: number; p50: number; p90: number; max: number; beliefHydrated: boolean }> {
  await resetDb();
  const gameId = `be${uuid().slice(0, 5)}`;
  const pids = [uuid(), uuid()];
  await seedGame(gameId, pids.map((id, i) => ({ id, name: `B${i}`, is_ai: true, strategy_key: strategy })));
  await executeWithGameLock(gameId, async (g: Game) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'bench-start', false);

  const samples: number[] = [];
  let beliefHydrated = false;
  let guard = 0;
  while (samples.length < MOVES && ++guard < MOVES * 6) {
    let acted = false;
    const t0 = nowMs();
    const { game } = await executeWithGameLock(gameId, async (g: Game) => {
      // executeWithGameLock already ran loadCompleteGame(g) — a real row read.
      if (loadSessionLogs) {
        (g as unknown as { belief_logs?: unknown[] }).belief_logs = await loadSessionLogs(gameId, g.players);
        if (((g as unknown as { belief_logs?: unknown[] }).belief_logs?.length ?? 0) > 0) beliefHydrated = true;
      }
      if (g.status !== GAME_STATUS.PLAYING) return { game: g, events: [] };
      for (let i = 0; i < g.players.length; i++) {
        const p = g.players[i];
        if (!p.is_ai || p.status !== PLAYER_STATUS.IN) continue;
        if (!shouldBotActCore(g, p, i)) continue;
        if (calculateLegalMoves(g, p.player_id).length === 0) continue;
        const res = await processBotActionPacked(g, p);
        if (res && res.run) {
          acted = true;
          const r = res.run;
          return {
            game: g, events: [],
            packed: {
              ended: r.ended,
              stateHex: bytesToHex(r.stateBlob),
              logsHex: bytesToBareHex(logsFromKernelExport(r.logsWire, Date.now())) || null,
              nEvents: r.nEvents,
              events: r.events ?? new Map<number, Uint8Array>(),
            },
          };
        }
        if (res) { acted = true; return { game: g, events: [] }; } // passive (good/wait), no packed
      }
      return { game: g, events: [] };
    }, 'bench', false);
    const dt = nowMs() - t0;

    // Only time real committed decisions; skip no-op cycles (round transitions
    // where no seat was eligible) so they don't deflate the latency.
    if (acted) samples.push(dt);
    if (game.status === GAME_STATUS.GAME_OVER) break;
    if (!acted) break; // nothing left to do and not over → avoid spinning
  }

  samples.sort((a, b) => a - b);
  const mean = samples.reduce((x, y) => x + y, 0) / (samples.length || 1);
  const pick = (q: number) => samples[Math.min(samples.length - 1, Math.floor(q * samples.length))] ?? 0;
  return { strategy, n: samples.length, mean, p50: pick(0.5), p90: pick(0.9), max: samples[samples.length - 1] ?? 0, beliefHydrated };
}

async function main() {
  try {
    const u = await import('../supabase/functions/_shared/utils.ts');
    if (typeof (u as Record<string, unknown>).loadSessionLogs === 'function') {
      loadSessionLogs = (u as Record<string, unknown>).loadSessionLogs as typeof loadSessionLogs;
    }
  } catch { /* older tree without the helper */ }

  await applySchema();
  // Deterministic deals/refills + MC streams so base-vs-head time the SAME work.
  __setKernelSeedSource(mkLcgU32(0x0C704E7));
  __setBotSeedSource(mkLcgU32(0xBEEF11));
  const results = [];
  try {
    for (const strat of BOTS) results.push(await benchStrategy(strat));
  } finally {
    __setKernelSeedSource(null); __setBotSeedSource(null);
  }

  // Peak wasm linear memory after the MC bots ran (the external-budget number).
  const memory = { botsWasmMB: __botsWasmMB(), kernelWasmMB: __kernelWasmMB() };

  if (JSON_OUT) { say(JSON.stringify({ e2e: results, memory })); await pgPool.end(); return; }
  say(`thinking-bot E2E latency vs real Postgres (load → belief → kernel choose → apply → commit), ${MOVES} decisions/bot`);
  say(`belief hydrated: ${results.some(r => r.beliefHydrated) ? 'yes' : 'no (blind — no loadSessionLogs on this tree)'}`);
  for (const r of results) {
    say(`  ${r.strategy.padEnd(10)} n=${String(r.n).padStart(3)}  mean ${r.mean.toFixed(1).padStart(6)}ms   p50 ${r.p50.toFixed(1).padStart(6)}ms   p90 ${r.p90.toFixed(1).padStart(6)}ms   max ${r.max.toFixed(0)}ms`);
  }
  say(`wasm memory: bots=${memory.botsWasmMB}MB kernel=${memory.kernelWasmMB}MB`);
  await pgPool.end();
}

main().catch((e) => { console.error(e); process.exit(1); });
