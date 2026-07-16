/* =============================================================================
 * Replay Format 6 (hidden-state-lossless, partial-game) — e2e through the wasm
 * =============================================================================
 * Plays real TS-engine games (the same engine replay_codec.test.ts uses, whose
 * game.logs carry REAL draw cards), captures the true initial deal, then:
 *
 *   initial hands + real logs -> encodeReplayV6 (wasm wasm_replay_encode_v6)
 *                             -> decodeReplay   (version-dispatched wasm decode)
 *
 * and asserts the decoded stream is fully identity-resolved: the leading
 * per-seat LOG_DRAWs are the true initial hands, every later LOG_DRAW carries a
 * REAL card (never hidden), the info actions round-trip, and a MID-GAME cut
 * decodes cleanly with no fool. This is the wasm/bridge proof on top of the
 * exhaustive native test (cnitro/tests/replay_v6_test.c).
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
import { encodeReplayV6, ReplayInputV6 } from '../supabase/functions/_shared/replay/encode.ts';
import { decodeReplay } from '../supabase/functions/_shared/replay/decode.ts';
import { buildReplaySteps } from '../src/replay/view.ts';
import { encodeLogsWire } from '../src/oracle/logsWire.ts';
import { __LOG_TYPE_TO_INT } from '../supabase/functions/_shared/wasm/engine.ts';

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
const tableCards = (g: Game): Card[] => g.table_battles.flatMap((b) =>
  b.defense ? [b.attack, b.defense] : [b.attack]);

// Play to completion. The TS engine masks DRAW cards in game.logs (and the
// kernel draws from a random deck index, so the deck array isn't the draw
// order), but the kernel exposes REAL hands every step — so we recover each
// draw's true cards exactly from the seat's hand delta and patch them into the
// DRAW logs, yielding the unmasked stream a v6 producer must feed the encoder.
async function playGame(np: number): Promise<{ game: Game; initialHands: Card[][]; flipped: Card; stock: Card[] } | null> {
  const game = mkGame(np);
  start_game(game);
  // The flip (and power_suit) are cleared once the trump is drawn late-game, so
  // snapshot the trump card now.
  const flipped: Card = { suit: game.flipped!.suit, value: game.flipped!.value };
  const flipKey = cardKey(flipped);
  const initialHands = game.players.map((p) => p.hand.map((c) => ({ suit: c.suit, value: c.value })));
  const stock: Card[] = []; // real drawn cards, in draw order (flip excluded)
  let actions = 0;
  while (game_done(game) === null) {
    if (++actions > MAX_ACTIONS) return null;
    const elig: PrivatePlayer[] = [];
    for (let i = 0; i < game.players.length; i++) {
      const p = game.players[i];
      if (shouldBotActCore(game, p, i) && calculateLegalMoves(game, p.player_id).length > 0) elig.push(p);
    }
    if (elig.length === 0) return null;
    for (let i = elig.length - 1; i > 0; i--) {
      const j = Math.floor(Math.random() * (i + 1));
      [elig[i], elig[j]] = [elig[j], elig[i]];
    }
    const before = game.players.map((pl) => new Set(pl.hand.map(cardKey)));
    const tableBefore = new Set(tableCards(game).map(cardKey));
    const logsBefore = game.logs.length;
    let acted = false;
    for (const p of elig) if (await processBotAction(game, p)) { acted = true; break; }
    if (!acted) return null;
    // Recover each DRAW's real cards (the seat's deck-sourced additions) in log
    // order = draw order, and accumulate the stock the v6 encoder consumes.
    for (let k = logsBefore; k < game.logs.length; k++) {
      const l = game.logs[k];
      if (l.log_type !== LOG_TYPE.DRAW) continue;
      const seat = game.players.findIndex((pl) => pl.player_id === l.player_id);
      const drawn = game.players[seat].hand.filter(
        (c) => !before[seat].has(cardKey(c)) && !tableBefore.has(cardKey(c)));
      if (drawn.length !== l.card_pairs.length) return null; // unexpected; skip game
      for (const c of drawn) if (cardKey(c) !== flipKey) stock.push({ suit: c.suit, value: c.value });
    }
  }
  return { game, initialHands, flipped, stock };
}

function inputOf(game: Game, initialHands: Card[][], flipped: Card, stock: Card[]): ReplayInputV6 {
  return {
    playerIds: game.players.map((p) => p.player_id),
    logs: game.logs,
    flipped,
    initialHands,
    stock,
  };
}

// A9: three tests stood here — "v6 wasm round-trips full games with real hidden
// state", "v6 wasm encodes a MID-GAME cut that decodes with no fool", and "v6
// finalize path: seed + masked logs -> exact hands". They are gone because
// cnitro/tests/replay_v6_test.c asserts all three ON THE KERNEL, natively:
// every hidden card's real identity survives an engine game's encode->decode,
// a mid-game prefix decodes with no fool, and replay_encode_v6_from_game is
// byte-equal to the marshalled producer. Re-asserting that through a TS bridge
// proved nothing extra and made every codec change a two-language edit.
//
// What is left below is what C cannot see: the WEB's consumption of a v6 code —
// the belief wire's DRAW masking and view.ts's fully-resolved hands. Both still
// reach the codec through encodeReplayV6, which is why the frozen choreography
// (collectV6 / marshalInputV6 / reconstructSeededDeal) is still alive. It
// retires WITH A5's web consumer: view.ts is the thing being deleted there, so
// these two tests and the choreography die in the same change, not before.

test('v6 belief wire DRAW-masks — no drawn-card identity leaks to the Oracle', async () => {
  const drawInt = __LOG_TYPE_TO_INT.get(LOG_TYPE.DRAW)!;
  let realDraws = 0, leaked = 0, checked = 0;
  for (let np = 2; np <= 4; np++) {
    for (let gi = 0; gi < GAMES_PER_PC; gi++) {
      const r = await playGame(np);
      if (!r || r.game.logs.length === 0) continue;
      const enc = await encodeReplayV6(inputOf(r.game, r.initialHands, r.flipped, r.stock));
      const dec = await decodeReplay(enc.x);
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
      const r = await playGame(np);
      if (!r) continue;
      const { game, initialHands, flipped, stock } = r;
      if (game.logs.length === 0) continue;

      const enc = await encodeReplayV6(inputOf(game, initialHands, flipped, stock));
      const dec = await decodeReplay(enc.x);
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
  assert.ok(checked > 0, 'exercised at least one game');
});
