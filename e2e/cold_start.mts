// Cold-start simulation: one fresh process = one cold Deno edge invocation that
// must LOAD the bots module (decode b64 -> compile -> instantiate) and make ONE
// cordite decision. argv[2] = raw .wasm path (for isolated compile timing).
// Reports: compile-only ms (scales with module size), and end-to-end cold
// first-decision ms via the REAL bridge (b64 decode + compile + instantiate +
// marshal + decide). This is the metric that decides O3-vs-Oz for edge.
import { readFileSync } from 'node:fs';
import { game_done } from '../supabase/functions/_shared/common/common_utils.ts';
import { start_game } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/common/bot_strategy.ts';
import { shouldBotActCore } from '../supabase/functions/_shared/common/pure_bot_actions.ts';
import { STRAT, wasmChooseMoveDirect, __setBotSeedSource, __botsWasmMB } from '../sdk/ts/wasm/bots.ts';
import { __setKernelSeedSource } from '../sdk/ts/wasm/engine.ts';
import { Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../supabase/functions/_shared/core/types.ts';
const __log = console.log.bind(console); console.log = () => {}; console.warn = () => {};

const mkLcgU32 = (s0: number) => { let s = (s0 >>> 0) || 1; return () => (s = (Math.imul(s, 1664525) + 1013904223) >>> 0); };
const mkGame = (np: number): Game => ({
  players: Array.from({ length: np }, (_, i): PrivatePlayer => ({ player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: STRATEGY_KEY.CORDITE })),
  deck: [], logs: [], id: 'cold', name: 'cold', status: GAME_STATUS.PLAYING, deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [], good_timestamp: null, good_players: [],
});

// (a) isolated cold compile cost (pure size effect)
const bytes = readFileSync(process.argv[2]);
const c0 = process.hrtime.bigint();
new WebAssembly.Module(bytes);
const compileMs = Number(process.hrtime.bigint() - c0) / 1e6;

// (b) end-to-end cold first decision via the real bridge
__setKernelSeedSource(mkLcgU32(0xDEA1));
const g = mkGame(6); start_game(g);
let seat = 0; for (let i = 0; i < 6; i++) { if (shouldBotActCore(g, g.players[i], i) && calculateLegalMoves(g, g.players[i].player_id).length) { seat = i; break; } }
const p = g.players[seat];
__setBotSeedSource(() => 123);
const e0 = process.hrtime.bigint();
wasmChooseMoveDirect(g, p.player_id, STRAT.cordite);   // triggers cold instantiate + 1 decision
const coldMs = Number(process.hrtime.bigint() - e0) / 1e6;
// warm: 10 more decisions on the same (now-loaded, tiering-up) module
let warmNs = 0, wc = 0;
for (let d = 0; d < 10; d++) {
  let did = false;
  for (let i = 0; i < 6; i++) {
    const q = g.players[i];
    if (!shouldBotActCore(g, q, i)) continue;
    if (calculateLegalMoves(g, q.player_id).length === 0) continue;
    __setBotSeedSource(() => 123 + d);
    const s = process.hrtime.bigint();
    const mv = wasmChooseMoveDirect(g, q.player_id, STRAT.cordite);
    warmNs += Number(process.hrtime.bigint() - s); wc++;
    __setBotSeedSource(null);
    const { executeBotMove } = await import('../supabase/functions/_shared/common/pure_bot_actions.ts');
    if (mv && executeBotMove(g, q, mv) !== false) { did = true; break; }
  }
  if (!did || game_done(g) !== null) break;
}
const warmMs = wc ? warmNs / wc / 1e6 : 0;
__log('COLD ' + JSON.stringify({ compileMs: +compileMs.toFixed(2), coldMs: +coldMs.toFixed(2), warmMs: +warmMs.toFixed(2), mem: __botsWasmMB() }));
