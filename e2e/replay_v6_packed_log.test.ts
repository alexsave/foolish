/* =============================================================================
 * v6 finalize: the packed session log must encode the same code as the objects
 * =============================================================================
 * docs/C_CORE_CONSOLIDATION.md F5/A4, trimmed to invariants by A9.
 *
 * This was the A4 differential harness: it kept ~390 lines of TS v6
 * choreography (reconstructSeededDeal + collectV6 + marshalInputV6) alive with
 * no production caller, purely to assert the kernel's one call was byte-equal
 * to it. That job is finished, and it is now the KERNEL's to keep:
 * sdk/c/tests/replay_v6_test.c asserts replay_encode_v6_from_game is
 * byte-equal to the marshalled producer, on real engine games — the same claim,
 * without a second implementation to maintain. Keeping the TS copy could only
 * ever prove "the copy agrees"; it could never prove the answer is right, and
 * it made every codec change a two-language edit (A9).
 *
 * What CANNOT move to C, and is therefore all that is left here: the server has
 * no GameLog[] at finalize. It reads games.logs_packed and splices those BYTES
 * into the kernel. Both calls below are the PRODUCTION producer — this pins the
 * TS-side log marshalling, not a re-implementation of the codec.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { game_done } from '../supabase/functions/_shared/common/common_utils.ts';
import { start_game } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import {
  Game, GAME_STATUS, PLAYER_STATUS, PrivatePlayer, STRATEGY_KEY,
} from '../supabase/functions/_shared/core/types.ts';
import { shouldBotActCore, processBotAction } from '../supabase/functions/_shared/common/pure_bot_actions.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/common/bot_strategy.ts';
import { __setDealSeedOverride } from '../sdk/ts/wasm/engine.ts';
import { kernelReplayEncodeV6FromGame } from '../sdk/ts/wasm/bots.ts';
import { encodeLogs } from '../sdk/ts/wire/logwire.ts';

if (!process.env.E2E_VERBOSE) {
  console.log = () => {}; console.warn = () => {}; console.error = () => {}; console.info = () => {};
}

const GAMES_PER_PC = Number(process.env.REPLAY_GAMES_PER_PC ?? 6);
const MAX_ACTIONS = 20000;

const mkGame = (np: number): Game => ({
  players: Array.from({ length: np }, (_, i): PrivatePlayer => ({
    player_id: `bot_${i}`, name: `Bot ${i}`, status: PLAYER_STATUS.READY,
    is_ai: true, hand: [], awaiting_attack: false, hand_length: 0,
    strategy_key: STRATEGY_KEY.RANDOM,
  })),
  deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
  first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
  good_timestamp: null, good_players: [],
});

const seedFor = (np: number, gi: number) =>
  Array.from({ length: 32 },
    (_, i) => ((i * 37 + gi * 101 + np * 7 + 3) & 0xff).toString(16).padStart(2, '0')).join('');

const hexToBytes = (h: string) => Uint8Array.from(h.match(/../g)!.map((b) => parseInt(b, 16)));

// A seeded game played to the end, exactly as the finalize path sees one: the
// game object plus its MASKED session log (draws hidden) plus games.game_seed.
async function playSeeded(np: number, gi: number): Promise<Game | null> {
  const seedHex = seedFor(np, gi);
  const game = mkGame(np);
  __setDealSeedOverride(hexToBytes(seedHex));
  try {
    start_game(game);
    let actions = 0;
    while (game_done(game) === null) {
      if (++actions > MAX_ACTIONS) return null;
      const elig: PrivatePlayer[] = [];
      for (let i = 0; i < game.players.length; i++) {
        const p = game.players[i];
        if (shouldBotActCore(game, p, i) && calculateLegalMoves(game, p.player_id).length > 0) elig.push(p);
      }
      if (elig.length === 0) return null;
      let acted = false;
      for (const p of elig) if (await processBotAction(game, p)) { acted = true; break; }
      if (!acted) return null;
    }
  } finally {
    __setDealSeedOverride(null);
  }
  if (!game.game_seed || game.logs.length === 0) return null;
  return game;
}

test('v6 finalize: the packed session log encodes the same code as the objects', async () => {
  // The server does not have GameLog[] at finalize — it reads games.logs_packed
  // and splices those bytes straight into the kernel. Same game, same seed, and
  // the log arrives as bytes instead of objects: the code must not move.
  let checked = 0;
  for (let np = 2; np <= 4; np++) {
    for (let gi = 0; gi < GAMES_PER_PC; gi++) {
      const game = await playSeeded(np, gi);
      if (!game) continue;

      const seatOf = (pid: string | null) => game.players.findIndex((p) => p.player_id === pid);
      const packed = encodeLogs(game.logs, seatOf);

      const viaObjects = kernelReplayEncodeV6FromGame(game, hexToBytes(game.game_seed!));
      const viaBytes = kernelReplayEncodeV6FromGame(game, hexToBytes(game.game_seed!), packed);

      assert.deepEqual(Array.from(viaBytes), Array.from(viaObjects),
        `np=${np} gi=${gi}: packed-log path differs from the object path`);
      checked++;
    }
  }
  assert.ok(checked > 0, 'exercised at least one packed-log game');
});
