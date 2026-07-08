// Equivalence gate for the C-buffer belief path (importLogsPacked).
//
// The server bot loop now hands the belief bots the session log as its RAW
// PACKED BYTES (game.belief_log_bytes, logwire format) and the kernel importer
// splices them straight in — no JS GameLog[] decode/marshal. This test proves
// that byte path is BEHAVIOR-IDENTICAL to the object path (game.belief_logs):
// same kernel, same seed, same position, the two must choose the exact same
// move. If the byte splice ever drifts from the object marshal (a card/seat/
// timestamp mislayout), the decisions diverge and this fails.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { game_done } from '../supabase/functions/_shared/common_utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/bot_strategy.ts';
import { shouldBotActCore, executeBotMove } from '../supabase/functions/_shared/pure_bot_actions.ts';
import { STRAT, wasmChooseMoveDirect, __setBotSeedSource } from '../supabase/functions/_shared/wasm/bots.ts';
import { __setKernelSeedSource } from '../supabase/functions/_shared/wasm/engine.ts';
import { encodeLogs } from '../supabase/functions/_shared/wire/logwire.ts';
import { Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS } from '../supabase/functions/_shared/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const mkLcgU32 = (seed: number) => {
  let s = (seed >>> 0) || 1;
  return () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s; };
};
const mkPlayer = (i: number): PrivatePlayer => ({
  player_id: `p${i}`, name: `octogen${i}`, status: PLAYER_STATUS.READY, is_ai: true,
  hand: [], awaiting_attack: false, hand_length: 0, strategy_key: 'octogen',
});
const mkGame = (np: number, gi: number): Game => ({
  players: Array.from({ length: np }, (_, i) => mkPlayer(i)),
  deck: [], logs: [], id: `bytes-${np}-${gi}`, name: 'bytes', status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
  first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
  good_timestamp: null, good_players: [],
});
const move = (g: Game, seat: number, seed: number) => {
  __setBotSeedSource(() => seed);
  const m = wasmChooseMoveDirect(g, g.players[seat].player_id, STRAT.octogen);
  __setBotSeedSource(null);
  return JSON.stringify(m);
};

test('belief_log_bytes (C-buffer path) chooses identically to belief_logs (objects)', () => {
  const metaSeed = mkLcgU32(0xBC0FFEE);
  __setKernelSeedSource(mkLcgU32(0xF00D));
  let compared = 0, mismatch = 0, examples: string[] = [];
  try {
    for (const np of [2, 3, 4]) {
      for (let gi = 0; gi < 6; gi++) {
        const g = mkGame(np, gi);
        start_game(g);
        let guard = 0;
        while (game_done(g) === null && ++guard < 3000) {
          let advanced = false;
          for (let i = 0; i < g.players.length; i++) {
            const p = g.players[i];
            if (p.status !== PLAYER_STATUS.IN || !shouldBotActCore(g, p, i)) continue;
            const legal = calculateLegalMoves(g, p.player_id);
            if (legal.length === 0) continue;

            const session = g.logs.slice();
            const seatOf = (pid: string | null) => pid === null ? -1 : g.players.findIndex(pl => pl.player_id === pid);
            const bytes = encodeLogs(session, seatOf);
            const seed = metaSeed();

            // object path
            g.belief_logs = session; g.belief_log_bytes = undefined; g.logs = [];
            const viaObjects = move(g, i, seed);
            // C-buffer path
            g.belief_logs = undefined; g.belief_log_bytes = bytes; g.logs = [];
            const viaBytes = move(g, i, seed);

            if (viaObjects !== viaBytes) {
              mismatch++;
              if (examples.length < 8) examples.push(`np=${np} d=${compared}: obj=${viaObjects} bytes=${viaBytes}`);
            }
            compared++;

            // restore and advance along one consistent path
            g.belief_logs = undefined; g.belief_log_bytes = undefined; g.logs = session;
            if (!advanced) {
              let mv = legal.find(m => JSON.stringify(m) === viaObjects) ?? legal[0];
              if (mv.type === 'wait') mv = legal.find(m => m.type !== 'wait') ?? mv;
              if (executeBotMove(g, p, mv) !== false) advanced = true;
            }
          }
          if (!advanced) break;
        }
      }
    }
  } finally {
    __setBotSeedSource(null);
    __setKernelSeedSource(null);
  }

  console.error(`[belief_log_bytes] compared=${compared} mismatch=${mismatch}`);
  assert.ok(compared > 500, `exercised ${compared} decisions`);
  assert.equal(mismatch, 0, `byte path diverged from object path on ${mismatch}/${compared}:\n${examples.join('\n')}`);
});
