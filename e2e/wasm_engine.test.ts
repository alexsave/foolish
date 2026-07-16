// The C rules kernel (cnitro/src/game.c + legal.c, compiled to WASM) is the
// single source of truth for gameplay; the TS modules in _shared delegate to
// it. This file guards the seams of that arrangement:
//
//   1. the kernel obeys THE deck-size rule (2..5 players -> 36 cards,
//      6+ -> 52), settled once for every deployment (see cnitro/src/card.h);
//   2. full random games through the kernel conserve cards and end with a
//      single fool at every player count;
//   3. the few thin TS projections kept for the client's synchronous use
//      (canCover, game_done, get_next_player_index, shouldBotActCore) never
//      drift from their kernel counterparts;
//   4. hostile inputs still reject with the production error messages.
//
// Pure kernel test — needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { game_done, canCover, get_next_player_index, cloneGame } from '../supabase/functions/_shared/common_utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import { handleAttack } from '../supabase/functions/_shared/actions/attack.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/bot_strategy.ts';
import {
  shouldBotActCore, processBotAction,
} from '../supabase/functions/_shared/pure_bot_actions.ts';
import {
  kernelGameDone, kernelShouldAct, kernelNextPlayer, kernelCanCover,
} from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';
import {
  Game, PrivatePlayer, Card, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
} from '../supabase/functions/_shared/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const mkPlayer = (i: number): PrivatePlayer => ({
  player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true,
  hand: [], awaiting_attack: false, hand_length: 0, strategy_key: STRATEGY_KEY.RANDOM,
});
const mkGame = (np: number): Game => ({
  players: Array.from({ length: np }, (_, i) => mkPlayer(i)),
  deck: [], logs: [], id: 'wasmtest', name: 'wasmtest', status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
  first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
  good_timestamp: null, good_players: [],
});

const countCards = (g: Game): number =>
  g.deck.length + (g.flipped ? 1 : 0) + g.discard_pile_length
  + g.players.reduce((a, p) => a + p.hand.length, 0)
  + g.table_battles.reduce((a, b) => a + 1 + (b.defense ? 1 : 0), 0);

test('kernel deals the settled deck size at every player count (6+ -> 52)', () => {
  for (let np = 2; np <= 8; np++) {
    const g = mkGame(np);
    start_game(g);
    const expected = np >= 6 ? 52 : 36;
    assert.equal(countCards(g), expected, `${np}p deals ${expected} cards`);
    assert.ok(g.flipped === null || g.flipped.value !== 13, 'flipped trump is never an Ace');
  }
});

test('kernel-driven random games conserve cards and end with one fool (2..8p)', async () => {
  for (let np = 2; np <= 8; np++) {
    const g = mkGame(np);
    start_game(g);
    const total = countCards(g);
    let guard = 0;
    while (game_done(g) === null && ++guard < 5000) {
      const actor = g.players.find((p, i) =>
        shouldBotActCore(g, p, i) && calculateLegalMoves(g, p.player_id).length > 0);
      if (!actor) break;
      assert.ok(await processBotAction(g, actor), 'eligible actor acts');
      assert.equal(countCards(g), total, 'card conservation');
    }
    assert.notEqual(game_done(g), null, `${np}p game finishes`);
    assert.equal(g.elimination_order.length, np - 1, 'everyone but the fool got out');
  }
});

test('the retained TS projections match the kernel on live states', async () => {
  for (let np = 2; np <= 6; np++) {
    const g = mkGame(np);
    start_game(g);
    let guard = 0;
    while (game_done(g) === null && ++guard < 400) {
      assert.equal(game_done(g), kernelGameDone(g), 'game_done parity');
      for (let i = 0; i < np; i++) {
        assert.equal(
          shouldBotActCore(g, g.players[i], i),
          kernelShouldAct(g, g.players[i].player_id),
          `shouldBotAct parity seat ${i}`,
        );
        if (g.players[i].status === PLAYER_STATUS.IN) {
          assert.equal(get_next_player_index(g, i), kernelNextPlayer(g, i), 'next-player parity');
        }
      }
      const actor = g.players.find((p, i) =>
        shouldBotActCore(g, p, i) && calculateLegalMoves(g, p.player_id).length > 0);
      if (!actor) break;
      await processBotAction(g, actor);
    }
  }

  // canCover over the full card cross-product
  for (let ps = 0; ps < 4; ps++) {
    for (let as = 0; as < 4; as++) for (let av = 1; av <= 13; av++) {
      for (let ds = 0; ds < 4; ds++) for (let dv = 1; dv <= 13; dv++) {
        const a: Card = { suit: as, value: av }, d: Card = { suit: ds, value: dv };
        assert.equal(canCover(a, d, ps), kernelCanCover(a, d, ps), 'canCover parity');
      }
    }
  }
});

test('hostile inputs reject with the production messages', () => {
  const g = mkGame(3);
  start_game(g);
  const attacker = g.players[g.first_attacker];
  const notMine: Card = g.players[(g.first_attacker + 2) % 3].hand[0];
  assert.throws(() => handleAttack(g, attacker.player_id, [notMine]), /not in/i, 'forged card');
  const mine = attacker.hand[0];
  assert.throws(() => handleAttack(g, attacker.player_id, [{ ...mine }, { ...mine }]), /duplicate/i, 'duplicate');
  assert.throws(() => handleAttack(g, 'ghost', [mine]), /not found in game/i, 'non-member');
  assert.throws(() => handleAttack(g, attacker.player_id, 'junk' as unknown as Card[]), /must be an array/i, 'malformed payload');
  const before = cloneGame(g);
  try { handleAttack(g, attacker.player_id, [notMine]); } catch { /* expected */ }
  assert.deepEqual(g.table_battles, before.table_battles, 'rejection never mutates');
});
