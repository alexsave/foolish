// Perf/mem harness for the wasm bot module: plays deterministic cordite-vs-
// cordite games through the REAL TS bridge (wasmChooseMoveDirect) and reports
// ns/decision + peak linear-memory MB. Seeds are pinned so every build variant
// runs the byte-identical workload (same deals, same MC rollouts) — the only
// thing that changes between runs is the swapped-in bots.wasm.
import { game_done } from '../supabase/functions/_shared/common/common_utils.ts';
import { start_game } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/common/bot_strategy.ts';
import { shouldBotActCore, executeBotMove } from '../supabase/functions/_shared/common/pure_bot_actions.ts';
import { STRAT, wasmChooseMoveDirect, __setBotSeedSource, __botsWasmMB } from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { __setKernelSeedSource } from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';
import { Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../supabase/functions/_shared/core/types.ts';

const __log = console.log.bind(console);
console.log = () => {}; console.warn = () => {}; console.info = () => {};

const mkLcgU32 = (seed: number) => { let s = (seed >>> 0) || 1; return () => (s = (Math.imul(s, 1664525) + 1013904223) >>> 0); };
const mkPlayer = (i: number): PrivatePlayer => ({
  player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true,
  hand: [], awaiting_attack: false, hand_length: 0, strategy_key: STRATEGY_KEY.CORDITE,
});
const mkGame = (np: number): Game => ({
  players: Array.from({ length: np }, (_, i) => mkPlayer(i)),
  deck: [], logs: [], id: 'bench', name: 'bench', status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
  first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
  good_timestamp: null, good_players: [],
});

const PCS = [2, 4, 6, 8];
const GAMES = 2;
__setKernelSeedSource(mkLcgU32(0xDEA1));       // deterministic deals/refills — identical every variant
const botSeed = mkLcgU32(0x12345);              // deterministic per-decision MC seed

let decisions = 0, totalNs = 0, peakMB = 0;
const wall0 = process.hrtime.bigint();
for (const np of PCS) {
  for (let gi = 0; gi < GAMES; gi++) {
    const g = mkGame(np);
    start_game(g);
    let guard = 0;
    while (game_done(g) === null && ++guard < 2000) {
      let advanced = false;
      for (let i = 0; i < np; i++) {
        const p = g.players[i];
        if (!shouldBotActCore(g, p, i)) continue;
        const lm = calculateLegalMoves(g, p.player_id);
        if (lm.length === 0) continue;
        const seed = botSeed();
        __setBotSeedSource(() => seed);
        const s = process.hrtime.bigint();
        const mv = wasmChooseMoveDirect(g, p.player_id, STRAT.cordite);
        totalNs += Number(process.hrtime.bigint() - s);
        __setBotSeedSource(null);
        decisions++;
        if (mv && executeBotMove(g, p, mv) !== false) { advanced = true; break; }
      }
      if (!advanced) break;
    }
    const mb = __botsWasmMB(); if (mb > peakMB) peakMB = mb;
  }
}
const wallMs = Math.round(Number(process.hrtime.bigint() - wall0) / 1e6);
__log('BENCH ' + JSON.stringify({ decisions, ns_per_decision: Math.round(totalNs / decisions), peakMB, wallMs }));
