/* =============================================================================
 * v6 as the WEB consumes it
 * =============================================================================
 * What a v6 code IS, and that it round-trips every hidden card, is asserted
 * natively by cnitro/tests/replay_v6_test.c — on real engine games, against the
 * kernel's own two producers. This file used to re-run all of that through a TS
 * bridge, and kept ~390 lines of TS choreography (reconstructSeededDeal +
 * collectV6 + marshalInputV6) alive to do it. Both are gone (A9): a second
 * implementation kept byte-identical by a parity test can only ever say "the
 * copy agrees", never "the answer is right".
 *
 * What is left is the part C cannot see — the WEB's consumption of a v6 code:
 *
 *   1. the belief wire must DRAW-mask, or a v6 replay leaks drawn-card
 *      identities to the Oracle that a live game would never reveal; and
 *   2. view.ts must build fully-resolved hands (the Oracle fix): v6 hides
 *      nothing, so no slot may be a retrodicted guess.
 *
 * Both now source their code from the PRODUCTION producer
 * (kernelReplayEncodeV6FromGame) — the same one call finalizeEndedGame makes, so
 * what is tested here is what actually ships.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { game_done } from '../supabase/functions/_shared/common_utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import {
  Card, Game, GAME_STATUS, PLAYER_STATUS, PrivatePlayer, STRATEGY_KEY, LOG_TYPE,
} from '../supabase/functions/_shared/types.ts';
import { shouldBotActCore, processBotAction } from '../supabase/functions/_shared/pure_bot_actions.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/bot_strategy.ts';
import { decodeReplay } from '../supabase/functions/_shared/replay/decode.ts';
import { kernelReplayEncodeV6FromGame } from '../supabase/functions/_shared/wasm/bots.ts';
import { bytesToBigint } from '../supabase/functions/_shared/replay/codec.ts';
import { __setDealSeedOverride, __LOG_TYPE_TO_INT } from '../supabase/functions/_shared/wasm/engine.ts';
import { buildReplaySteps } from '../src/replay/view.ts';
import { encodeLogsWire } from '../src/oracle/logsWire.ts';

if (!process.env.E2E_VERBOSE) {
  console.log = () => {}; console.warn = () => {}; console.error = () => {}; console.info = () => {};
}

const GAMES_PER_PC = Number(process.env.REPLAY_GAMES_PER_PC ?? 12);
const MAX_ACTIONS = 100000;

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

const cardKey = (c: Card) => `${c.suit}:${c.value}`;

const seedFor = (np: number, gi: number) =>
  Array.from({ length: 32 },
    (_, i) => ((i * 41 + gi * 97 + np * 11 + 5) & 0xff).toString(16).padStart(2, '0')).join('');

const hexToBytes = (h: string) => Uint8Array.from(h.match(/../g)!.map((b) => parseInt(b, 16)));

// A seeded game played to the end — exactly what finalizeEndedGame holds. The
// deal-seed override is load-bearing: the kernel re-derives the deal FROM the
// seed to encode, so without it the encoder rebuilds a different game and the
// logged actions do not fit its menus ("logged attack not in menu").
//
// Nothing is reconstructed here any more. The producer reads the deal from the
// seed and the actions out of the session log itself — which is the whole point
// of A4. This used to snapshot the flip, mirror every DRAW back into a stock
// array, and hand-assemble a reveal stream, just to have something to encode.
async function playSeeded(np: number, gi: number): Promise<Game | null> {
  const game = mkGame(np);
  __setDealSeedOverride(hexToBytes(seedFor(np, gi)));
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

/** The code the site really shares: ONE kernel call, from the game and its seed. */
const v6Of = (game: Game) =>
  decodeReplay(bytesToBigint(kernelReplayEncodeV6FromGame(game, hexToBytes(game.game_seed!))));

test('v6 belief wire DRAW-masks — no drawn-card identity leaks to the Oracle', async () => {
  const drawInt = __LOG_TYPE_TO_INT.get(LOG_TYPE.DRAW)!;
  let realDraws = 0, leaked = 0, checked = 0;
  for (let np = 2; np <= 4; np++) {
    for (let gi = 0; gi < GAMES_PER_PC; gi++) {
      const game = await playSeeded(np, gi);
      if (!game) continue;
      const dec = await v6Of(game);
      for (const l of dec.logs)                        // sanity: v6 draws ARE real
        if (l.log_type === LOG_TYPE.DRAW) for (const p of l.card_pairs) if (p.primary.suit >= 0) realDraws++;

      const wire = encodeLogsWire(dec.logs);           // the memory-on belief feed
      let pos = 0; const rd = () => wire[pos++];
      const n = rd() | (rd() << 8);
      for (let i = 0; i < n; i++) {
        const type = rd(); rd(); rd(); const npair = rd();
        for (let j = 0; j < npair; j++) { const prim = rd(); rd(); if (type === drawInt && prim !== 0xFE) leaked++; }
      }
      checked++;
    }
  }
  assert.ok(checked > 0 && realDraws > 0, 'exercised v6 replays with real draws');
  assert.equal(leaked, 0, `${leaked} drawn-card identities leaked into the belief wire`);
});

test('v6 view.ts builds fully-resolved hands (no retrodiction — the Oracle fix)', async () => {
  let checked = 0;
  for (let np = 2; np <= 4; np++) {
    for (let gi = 0; gi < GAMES_PER_PC; gi++) {
      const game = await playSeeded(np, gi);
      if (!game) continue;
      const dec = await v6Of(game);
      const steps = buildReplaySteps(dec); // must not throw (conservation holds)

      // The whole point: a v6 replay is fully identity-resolved. Hidden cards are
      // face-DOWN slots (so the deal doesn't render face-up — the UI fix), but
      // EVERY slot carries its true identity — the view/Oracle never retrodict.
      for (const step of steps)
        for (const p of step.players)
          assert.ok(p.slots.every((s) => s !== null),
            'every v6 hidden slot has a resolved identity (no retrodicted guess)');

      // Exactness: at the final step, each seat's FULL hand (public known +
      // face-down-but-resolved slots) equals its true hand.
      const last = steps[steps.length - 1];
      for (let s = 0; s < np; s++) {
        const full = [...last.players[s].known, ...last.players[s].slots.filter(Boolean) as Card[]];
        const got = new Set(full.map(cardKey));
        const want = new Set(game.players[s].hand.map(cardKey));
        assert.equal(got.size, want.size, `seat ${s} final hand size`);
        for (const k of want) assert.ok(got.has(k), `seat ${s} final hand card ${k}`);
      }
      checked++;
    }
  }
  assert.ok(checked > 0, 'exercised v6 replays through view.ts');
});
