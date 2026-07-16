// Regression guard for the belief-bot session-log wiring.
//
// The belief/memory bots (octogen, semtex, cordite, fulminate, espresso) deduce
// hidden cards from the current session log. The server hot-path loader
// (loadCompleteGame) deliberately leaves game.logs EMPTY, so for a long window
// these bots ran blind in production — importLogs marshaled zero records and the
// bots played as if they had no memory (see the octogen investigation). The fix
// carries the session log in a dedicated read-only field, game.belief_logs,
// which the bot loop hydrates from games.logs_packed before the kernel chooses.
//
// This test pins that contract, WITHOUT a database, at the kernel boundary:
//   1. belief_logs is honored IDENTICALLY to game.logs (the field the offline
//      harnesses populate) — same kernel, same seed, same position ⇒ same move.
//   2. the session log is LOAD-BEARING — octogen changes its move on a
//      meaningful fraction of positions when the log is present vs empty. If the
//      log were being ignored again (the regression), this count would be 0.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { game_done } from '../supabase/functions/_shared/common/common_utils.ts';
import { start_game } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/common/bot_strategy.ts';
import { shouldBotActCore, executeBotMove } from '../supabase/functions/_shared/common/pure_bot_actions.ts';
import { STRAT, wasmChooseMoveDirect, __setBotSeedSource } from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { __setKernelSeedSource } from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';
import {
  Game, PrivatePlayer, GameLog, PLAYER_STATUS, GAME_STATUS,
} from '../supabase/functions/_shared/core/types.ts';

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
  deck: [], logs: [], id: `belief-${np}-${gi}`, name: 'belief', status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
  first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
  good_timestamp: null, good_players: [],
});
const move = (g: Game, seat: number, strat: number, seed: number) => {
  __setBotSeedSource(() => seed);
  const m = wasmChooseMoveDirect(g, g.players[seat].player_id, strat);
  __setBotSeedSource(null);
  return JSON.stringify(m);
};

test('belief_logs is honored like game.logs AND is load-bearing for octogen', () => {
  const metaSeed = mkLcgU32(0xB3113F);
  __setKernelSeedSource(mkLcgU32(0xC0FFEE));
  let compared = 0, fieldMismatch = 0, beliefChangedMove = 0;
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

            // game.logs holds the real accumulated session (executeBotMove
            // appends via addLog). Snapshot it as the "belief input".
            const sessionLog: GameLog[] = g.logs.slice();
            const seed = metaSeed();

            // (1) belief_logs == game.logs: move via belief_logs (logs cleared)
            // must equal move via game.logs (belief_logs unset).
            g.belief_logs = sessionLog; g.logs = [];
            const viaBelief = move(g, i, STRAT.octogen, seed);
            g.belief_logs = undefined; g.logs = sessionLog;
            const viaGameLogs = move(g, i, STRAT.octogen, seed);
            if (viaBelief !== viaGameLogs) fieldMismatch++;

            // (2) load-bearing: with the log vs with an empty log.
            g.belief_logs = []; g.logs = [];
            const viaEmpty = move(g, i, STRAT.octogen, seed);
            if (viaGameLogs !== viaEmpty) beliefChangedMove++;

            g.belief_logs = undefined; g.logs = sessionLog; // restore for play
            compared++;

            if (!advanced) {
              let mv = legal.find(m => JSON.stringify(m) === viaGameLogs) ?? legal[0];
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

  console.error(`[belief_logs] compared=${compared} fieldMismatch=${fieldMismatch} beliefChangedMove=${beliefChangedMove}`);
  assert.ok(compared > 500, `exercised ${compared} decisions`);
  // (1) belief_logs must be a drop-in for game.logs — every position identical.
  assert.equal(fieldMismatch, 0, `belief_logs diverged from game.logs on ${fieldMismatch}/${compared} positions`);
  // (2) if the log were ignored (the regression), this would be exactly 0.
  assert.ok(beliefChangedMove > 0,
    `session log never changed octogen's move (${beliefChangedMove}/${compared}) — belief input is being ignored`);
});
