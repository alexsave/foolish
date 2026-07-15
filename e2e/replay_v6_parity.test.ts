/* =============================================================================
 * A4 differential harness — the TS v6 choreography vs the kernel's one call
 * =============================================================================
 * docs/C_CORE_CONSOLIDATION.md F5/A4. The standing migration playbook: before
 * finalizeEndedGame is cut over, prove the new producer is byte-identical to
 * the old one on real seeded games, and keep the old path as the oracle.
 *
 *   OLD (the oracle, ~390 lines across three modules, two wasm round-trips):
 *     reconstructSeededDeal(seed)  -> initial hands + stock + flip
 *     collectV6 / marshalInputV6   -> reveal stream + action stream + header
 *     kernelReplayEncodeV6(bytes)  -> the replay integer
 *
 *   NEW (one call):
 *     kernelReplayEncodeV6FromGame(game, seed, logs)
 *
 * The kernel re-derives the deal from the seed itself and reads the actions out
 * of the session log, so the reveal assembly and the action marshal have no
 * caller-side existence any more. Byte-equality is what makes that a PORT
 * rather than a second producer.
 *
 * Both of the server's log shapes are covered: the JS GameLog[] a test/offline
 * harness has, and the packed games.logs_packed BYTES the live finalize reads.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { game_done } from '../supabase/functions/_shared/common_utils.ts';
import { start_game, reconstructSeededDeal } from '../supabase/functions/_shared/game_lifecycle.ts';
import {
  Card, Game, GAME_STATUS, PLAYER_STATUS, PrivatePlayer, STRATEGY_KEY,
} from '../supabase/functions/_shared/types.ts';
import { shouldBotActCore, processBotAction } from '../supabase/functions/_shared/pure_bot_actions.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/bot_strategy.ts';
import { verifyRoundTripV6 } from '../supabase/functions/_shared/replay/encode.ts';
import { decodeReplay } from '../supabase/functions/_shared/replay/decode.ts';
import { __setDealSeedOverride } from '../supabase/functions/_shared/wasm/engine.ts';
import { kernelReplayEncodeV6FromGame } from '../supabase/functions/_shared/wasm/bots.ts';
import { encodeLogs } from '../supabase/functions/_shared/wire/logwire.ts';

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

// The oracle: literally what finalizeEndedGame does today (utils.ts:962-974).
async function oracleV6(game: Game): Promise<Uint8Array> {
  const { initialHands, stock, flip } = reconstructSeededDeal(game.game_seed!, game.players);
  const { encoded } = await verifyRoundTripV6({
    playerIds: game.players.map((p) => p.player_id),
    logs: game.logs, flipped: flip, initialHands, stock,
  });
  return encoded.bytes;
}

test('A4: kernel v6-from-game is byte-identical to the TS choreography', async () => {
  let checked = 0;
  for (let np = 2; np <= 4; np++) {
    for (let gi = 0; gi < GAMES_PER_PC; gi++) {
      const game = await playSeeded(np, gi);
      if (!game) continue;

      const want = await oracleV6(game);
      // NOTE: oracleV6 runs reconstructSeededDeal, which CLOBBERS the kernel's
      // resident state with its re-deal. The kernel call below marshals fresh,
      // which is the only reason that is survivable — and is why it does.
      const got = kernelReplayEncodeV6FromGame(game, hexToBytes(game.game_seed!));

      assert.equal(got.length, want.length,
        `np=${np} gi=${gi}: v6 length ${got.length} vs oracle ${want.length}`);
      assert.deepEqual(Array.from(got), Array.from(want),
        `np=${np} gi=${gi}: v6 bytes differ from the TS choreography`);
      checked++;
    }
  }
  assert.ok(checked > 0, 'exercised at least one seeded game');
});

test('A4: same bytes from the PACKED session log (the live finalize path)', async () => {
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

test('A4: the code really is v6, and names the right fool', async () => {
  const game = await playSeeded(3, 0);
  assert.ok(game, 'game played');
  const bytes = kernelReplayEncodeV6FromGame(game!, hexToBytes(game!.game_seed!));
  // decodeReplay wants the integer; encoded.x is what the codec round-trips on.
  let x = 0n;
  for (const b of bytes) x = (x << 8n) | BigInt(b);
  const dec = await decodeReplay(x);
  const foolSeat = game!.players.findIndex((p) => p.player_id === game_done(game!));
  assert.equal(dec.formatVersion, 6, 'format version');
  assert.equal(dec.fool, foolSeat, 'fool seat');
});

test('A4: a seed that did not deal this game is rejected, not encoded', async () => {
  // Without this the failure mode is silent corruption: the reveals would
  // describe a different deal and the encode could still succeed, storing a
  // replay of a game nobody played. The kernel checks the re-dealt trump
  // against the one the game carries.
  const game = await playSeeded(3, 1);
  assert.ok(game, 'game played');
  const bad = hexToBytes(game!.game_seed!);
  bad[0] ^= 0xff;
  // On a finished game the trump has been drawn, so the up-front trump check is
  // skipped and this fails deeper: the logged opening attack is not in the menu
  // the wrong deal produces ("logged action not in menu").
  assert.throws(() => kernelReplayEncodeV6FromGame(game!, bad),
    /not in menu|trump|replay/i, 'a wrong seed must not produce a code');
});
