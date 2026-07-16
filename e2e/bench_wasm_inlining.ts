// HELPER, not a test (no `.test.ts` suffix ⇒ the e2e glob skips it). Supports
// docs/WASM_OPT_INLINING_PROTOTYPE.md.
//
// Micro-benchmark: total wall time spent inside wasmChooseMove for a heavy
// MC bot (octogen), driving full games in the WASM kernel. Deterministic
// (pinned kernel + per-decision seeds), so baseline vs wasm-opt'd modules do
// the SAME work — only codegen differs. Run in separate processes, swapping
// bots.wasm.gz between runs.
//
//   GAMES=3 TSX_TSCONFIG_PATH=e2e/tsconfig.json \
//     node --import tsx e2e/bench_wasm_inlining.ts 2>/dev/null | grep '^{'
import { game_done } from '../supabase/functions/_shared/common/common_utils.ts';
import { start_game } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import { calculateLegalMoves, LegalMove } from '../supabase/functions/_shared/common/bot_strategy.ts';
import { shouldBotActCore, executeBotMove } from '../supabase/functions/_shared/common/pure_bot_actions.ts';
import { STRAT, wasmChooseMove, __setBotSeedSource } from '../sdk/ts/wasm/bots.ts';
import { __setKernelSeedSource } from '../sdk/ts/wasm/engine.ts';
import { PLAYER_STATUS, GAME_STATUS } from '../supabase/functions/_shared/core/types.ts';

const mkLcgU32 = (seed: number) => { let s = seed >>> 0; return () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s; }; };
const mkPlayer = (i: number) => ({ player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: 'octogen' } as any);
const mkGame = (np: number): any => ({ players: Array.from({ length: np }, (_, i) => mkPlayer(i)), deck: [], logs: [], id: 'bench', name: 'bench', status: GAME_STATUS.PLAYING, deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [], good_timestamp: null, good_players: [] });

const STRATEGY = STRAT.octogen;
const PLAYER_COUNTS = [2, 3, 4];
const GAMES_PER_COUNT = Number(process.env.GAMES ?? 8);

let choose_ns = 0n;
let decisions = 0;
__setKernelSeedSource(mkLcgU32(0xDEA1));
const metaSeed = mkLcgU32(0xB07);

const t0 = process.hrtime.bigint();
for (const np of PLAYER_COUNTS) {
  for (let gi = 0; gi < GAMES_PER_COUNT; gi++) {
    const g = mkGame(np); start_game(g);
    let guard = 0;
    while (game_done(g) === null && ++guard < 3000) {
      let advanced = false;
      for (let i = 0; i < np; i++) {
        const p = g.players[i];
        if (!shouldBotActCore(g, p, i)) continue;
        const legalMoves = calculateLegalMoves(g, p.player_id);
        if (legalMoves.length === 0) continue;
        const seed = metaSeed();
        __setBotSeedSource(() => seed);
        const c0 = process.hrtime.bigint();
        const wasmIdx = wasmChooseMove(g, p.player_id, STRATEGY);
        choose_ns += process.hrtime.bigint() - c0;
        __setBotSeedSource(null);
        decisions++;
        if (!advanced) {
          let mv: LegalMove | undefined = legalMoves[wasmIdx];
          if (mv?.type === 'wait') mv = legalMoves.find(m => m.type !== 'wait');
          if (mv && executeBotMove(g, p, mv) !== false) advanced = true;
        }
      }
      if (!advanced) break;
    }
  }
}
const wall_ms = Number(process.hrtime.bigint() - t0) / 1e6;
console.log(JSON.stringify({ decisions, choose_ms: +(Number(choose_ns) / 1e6).toFixed(1), wall_ms: +wall_ms.toFixed(1), ns_per_decision: Math.round(Number(choose_ns) / decisions) }));
