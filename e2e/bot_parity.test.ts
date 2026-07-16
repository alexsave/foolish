// Bot-choice parity: the C kernel strategies (cnitro/src/*_strategy.c via
// bots.wasm) vs the TS originals, move-for-move on identical positions with
// identical RNG streams. This is the proof that moving bot brains into the
// C kernel changed nothing about how they play.
//
// RNG pinning: before every decision both sides are pointed at the same LCG
// stream — Math.random (which the TS strategies consume) is patched to the
// kernel's strategy LCG (s = s*1664525 + 1013904223; s/2^32, see
// cnitro/src/game.c random_strategy_random) seeded identically via
// __setBotSeedSource.
//
// cordite/fulminate are exercised elsewhere: they are independent
// implementations of the same Monte-Carlo design (bitboard C vs SimGame TS),
// not line ports, so move-for-move equality is not the contract — legality,
// determinism-under-seed and strength are (see cnitro difftests + arena).
//
// Pure kernel test — needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { game_done } from '../supabase/functions/_shared/common/common_utils.ts';
import { start_game } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import { calculateLegalMoves, BotStrategy, LegalMove } from '../supabase/functions/_shared/common/bot_strategy.ts';
import { shouldBotActCore, executeBotMove } from '../supabase/functions/_shared/common/pure_bot_actions.ts';
import { STRAT, wasmChooseMove, wasmChooseMoveDirect, __setBotSeedSource } from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { __setKernelSeedSource } from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';
import {
  Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
} from '../supabase/functions/_shared/core/types.ts';
import { RandomBotStrategy, setRandomSeed } from '../offlinefun/localtest/frozen/random_strategy.ts';
import { HandwrittenBotStrategy } from '../offlinefun/localtest/frozen/handwritten_strategy.ts';
import { SimpleHeuristicStrategy } from '../offlinefun/localtest/frozen/simple_heuristic_strategy.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

// The kernel's dedicated strategy LCG (cnitro/src/game.c).
const mkLcg = (seed: number) => {
  let s = (seed >>> 0) || 1;
  return () => {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    return s / 4294967296;
  };
};
const mkLcgU32 = (seed: number) => {
  let s = (seed >>> 0) || 1;
  return () => {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    return s;
  };
};

// Seat strategy keys matter: espresso's opponent modeling branches on
// whether an opponent is the 'random' bot, so alternate games mix keys to
// cover both sides of that branch.
const KEY_MIXES = [
  () => STRATEGY_KEY.RANDOM,
  () => STRATEGY_KEY.HANDWRITTEN,
  (i: number) => (i % 2 ? STRATEGY_KEY.RANDOM : STRATEGY_KEY.SIMPLE_HEURISTIC),
];
const mkPlayer = (i: number, mix: number): PrivatePlayer => ({
  player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true,
  hand: [], awaiting_attack: false, hand_length: 0,
  strategy_key: KEY_MIXES[mix % KEY_MIXES.length](i),
});
const mkGame = (np: number, mix = 0): Game => ({
  players: Array.from({ length: np }, (_, i) => mkPlayer(i, mix)),
  deck: [], logs: [], id: 'botparity', name: 'botparity', status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
  first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
  good_timestamp: null, good_players: [],
});

interface ParityCase {
  name: string;
  ts: BotStrategy;
  strat: number;
  // Point the TS side at the shared per-decision LCG stream. Most strategies
  // consume Math.random; the TS random bot has its own module-level LCG.
  pin?: (seed: number) => void;
}

// Only the wasm-dispatchable ladder bots remain parity-checkable. STRAT.handwritten
// resolves to the shipped handwritten_prod kernel (bots.ts maps 'handwritten'->16),
// whose frozen TS mirror is HandwrittenBotStrategy. espresso (-> espresso_prod),
// champion, ultimate_champion and hacker were dropped from the wasm build
// entirely, so there is no kernel to compare them against here; their frozen TS
// mirrors + the native C suite still cover them. cordite/octogen are exercised
// elsewhere (they are re-implementations, not line ports — see header).
const CASES: ParityCase[] = [
  { name: 'random', ts: new RandomBotStrategy(), strat: STRAT.random, pin: setRandomSeed },
  { name: 'handwritten', ts: new HandwrittenBotStrategy(), strat: STRAT.handwritten },
  { name: 'simple_heuristic', ts: new SimpleHeuristicStrategy(), strat: STRAT.simple_heuristic },
];

const PLAYER_COUNTS = [2, 3, 4, 6, 8];
const GAMES_PER_COUNT = 6;

for (const { name, ts, strat, pin } of CASES) {
  test(`kernel ${name} chooses the exact TS move on every decision`, async () => {
    // Deterministic run: game randomness (deals, refills) comes from a pinned
    // kernel stream; strategy randomness from a fresh pinned seed per decision.
    let seedOf = 0;
    for (let i = 0; i < name.length; i++) seedOf = (Math.imul(seedOf, 31) + name.charCodeAt(i)) >>> 0;
    const metaSeed = mkLcgU32(0xB07 ^ seedOf);
    __setKernelSeedSource(mkLcgU32(0xDEA1 ^ seedOf));

    const realRandom = Math.random;
    let decisions = 0;
    try {
      for (const np of PLAYER_COUNTS) {
        for (let gi = 0; gi < GAMES_PER_COUNT; gi++) {
          const g = mkGame(np, gi);
          start_game(g);
          let guard = 0;
          while (game_done(g) === null && ++guard < 3000) {
            // Compare the choice of EVERY eligible seat at this state, then
            // advance with the first seat's (non-wait) choice.
            let advanced = false;
            for (let i = 0; i < np; i++) {
              const p = g.players[i];
              if (!shouldBotActCore(g, p, i)) continue;
              const legalMoves = calculateLegalMoves(g, p.player_id);
              if (legalMoves.length === 0) continue;

              const seed = metaSeed();
              if (pin) pin(seed);
              else Math.random = mkLcg(seed);
              const tsMove = await ts.chooseMove(g, p.player_id, legalMoves);
              Math.random = realRandom;
              const tsIdx = legalMoves.indexOf(tsMove);

              __setBotSeedSource(() => seed);
              const wasmIdx = wasmChooseMove(g, p.player_id, strat);
              // The bot loop's fast path decodes the chosen move from the
              // kernel's IO buffer instead of indexing this list — assert
              // the bytes decode to the exact same move.
              __setBotSeedSource(() => seed);
              const direct = wasmChooseMoveDirect(g, p.player_id, strat);
              __setBotSeedSource(null);
              assert.deepStrictEqual(
                direct, legalMoves[wasmIdx],
                `${name} direct-move decode mismatch at decision ${decisions}`,
              );

              assert.equal(
                wasmIdx, tsIdx,
                `${name} np=${np} game=${gi} decision=${decisions} seat=${i} `
                + `seed=${seed} moves=${legalMoves.length} `
                + `ts=${JSON.stringify(legalMoves[tsIdx])} wasm=${JSON.stringify(legalMoves[wasmIdx])}`,
              );
              decisions++;

              if (!advanced) {
                let mv: LegalMove | undefined = legalMoves[wasmIdx];
                if (mv.type === 'wait') mv = legalMoves.find(m => m.type !== 'wait');
                if (mv && executeBotMove(g, p, mv) !== false) advanced = true;
              }
            }
            if (!advanced) break;
          }
        }
      }
    } finally {
      Math.random = realRandom;
      __setBotSeedSource(null);
      __setKernelSeedSource(null);
    }
    assert.ok(decisions > 3000, `exercised ${decisions} decisions`);
  });
}
