// Client-side move gates (src/utils/gameValidation.ts). The rule gates now
// delegate to the kernel (guards.wasm) — e2e/client_guards fuzzes them against
// the authoritative server kernel across thousands of states. This file keeps
// hand-picked concrete cases (readable regressions) plus the one piece that is
// NOT a kernel rule: canCoverCards, the UI affordance that decides when to
// OFFER a one-click cover (unambiguous target set).
//
// The gates judge the LOCAL player's own move, so `self` must be the acting
// seat and actually hold the cards it plays (the kernel checks membership —
// the old hand-rolled TS gates did not).
//
// Pure client logic — needs no Postgres and no DOM.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  canAttack, canCoverCards, canPickup, validateAttack, validatePass, validatePickup, validateCover,
} from '../src/utils/gameValidation.ts';
import {
  PersonalGame, PublicPlayer, Card, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
} from '../server/api/core/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const C = (suit: number, value: number): Card => ({ suit, value });
const P = (i: number, handLen: number): PublicPlayer => ({
  name: `P${i}`, player_id: `p${i}`, status: PLAYER_STATUS.IN, hand_length: handLen, is_ai: false,
});

// Diamonds (3) trump; seat 0 attacks / seat 1 defends. `self` is the acting
// seat with a real hand.
const mkGame = (
  handLens: number[],
  table: PersonalGame['table_battles'],
  opts: { defenderHand?: number; selfSeat?: number; selfHand?: Card[] } = {},
): PersonalGame => {
  const { defenderHand = 6, selfSeat = 0, selfHand = [] } = opts;
  const players = handLens.map((n, i) => P(i, i === 1 ? defenderHand : n));
  const base = players[selfSeat];
  return {
    id: 'g', name: 'g', deck_length: 0, discard_pile_length: 0, flipped: null,
    players, status: GAME_STATUS.PLAYING, power_suit: 3, first_attacker: 0, defender: 1,
    table_battles: table, elimination_order: [], good_timestamp: null, good_players: [],
    self: {
      player_id: base.player_id, name: base.name, status: PLAYER_STATUS.IN, is_ai: false,
      hand: selfHand, awaiting_attack: false, hand_length: selfHand.length, strategy_key: STRATEGY_KEY.HUMAN,
    },
  };
};

// ---- canAttack --------------------------------------------------------------

test('canAttack: first attack requires held, same-value cards within defender capacity', () => {
  const hand = [C(0, 5), C(1, 5), C(1, 6)];
  const g = mkGame([6, 6], [], { selfHand: hand });
  assert.equal(canAttack(g, []), false, 'no cards -> false');
  assert.equal(canAttack(g, [C(0, 5), C(1, 5)]), true, 'a held same-value pair opens');
  assert.equal(canAttack(g, [C(0, 5), C(1, 6)]), false, 'mixed values cannot open');
  assert.equal(canAttack(g, [C(2, 5)]), false, 'a card not in hand cannot be played');

  const tight = mkGame([6, 6], [], { defenderHand: 1, selfHand: hand });
  assert.equal(canAttack(tight, [C(0, 5), C(1, 5)]), false, 'cannot exceed defender capacity');
});

test('canAttack: a follow-up attack must match a value already on the table', () => {
  const g = mkGame([6, 6], [{ attack: C(0, 7), defense: C(0, 9) }], { selfHand: [C(2, 7), C(2, 9), C(2, 8)] });
  assert.equal(canAttack(g, [C(2, 7)]), true, 'attack value present on the table (attack side)');
  assert.equal(canAttack(g, [C(2, 9)]), true, 'attack value present on the table (defense side)');
  assert.equal(canAttack(g, [C(2, 8)]), false, 'value not on the table');
});

// ---- canCoverCards (UI affordance, not a kernel rule) -----------------------

test('canCoverCards: single card is offered only when its target is unambiguous', () => {
  const two = mkGame([6, 6], [{ attack: C(0, 7), defense: null }, { attack: C(0, 8), defense: null }]);
  assert.equal(canCoverCards(two, [C(0, 9)]), false, 'a 9♠ covering both 7♠ and 8♠ is ambiguous');

  const one = mkGame([6, 6], [{ attack: C(0, 7), defense: null }]);
  assert.equal(canCoverCards(one, [C(0, 9)]), true, 'exactly one legal target -> offered');
  assert.equal(canCoverCards(one, [C(0, 6)]), false, 'a card that cannot cover -> not offered');
  assert.equal(canCoverCards(one, []), false, 'no selection -> false');
});

test('canCoverCards: multi-card cover is offered only when the mapping is unambiguous', () => {
  const unambiguous = mkGame([6, 6], [{ attack: C(0, 7), defense: null }, { attack: C(2, 8), defense: null }]);
  assert.equal(canCoverCards(unambiguous, [C(0, 9), C(2, 9)]), true, 'each cover fits exactly one attack');

  const ambiguous = mkGame([6, 6], [
    { attack: C(0, 7), defense: null }, { attack: C(0, 8), defense: null }, { attack: C(0, 9), defense: null },
  ]);
  assert.equal(canCoverCards(ambiguous, [C(3, 10), C(3, 11)]), false, 'two trumps over three attacks is ambiguous');
});

// ---- throwing validators (optimistic-apply pre-checks) ----------------------

test('validateAttack rejects over-capacity and off-table values, else passes', () => {
  const roomy = mkGame([6, 6], [{ attack: C(0, 7), defense: null }], { selfHand: [C(2, 7), C(2, 8)] });
  assert.doesNotThrow(() => validateAttack(roomy, [C(2, 7)]), 'held, value on table, room to defend');

  const tight = mkGame([6, 6], [{ attack: C(0, 7), defense: null }], { defenderHand: 1, selfHand: [C(2, 7)] });
  assert.throws(() => validateAttack(tight, [C(2, 7)]), 'uncovered + new > defender hand');

  assert.throws(() => validateAttack(roomy, [C(2, 8)]), 'off-table value rejected');
});

test('validatePass rejects mixed values and un-passable tables, else passes', () => {
  const g = mkGame([6, 6], [{ attack: C(0, 7), defense: null }], { selfSeat: 1, selfHand: [C(2, 7), C(1, 8)] });
  assert.doesNotThrow(() => validatePass(g, [C(2, 7)]), 'defender holds a 7, single uncovered 7 on the table');
  assert.throws(() => validatePass(g, [C(2, 7), C(1, 8)]), 'mixed pass values');

  const covered = mkGame([6, 6], [{ attack: C(0, 7), defense: C(0, 9) }], { selfSeat: 1, selfHand: [C(2, 7)] });
  assert.throws(() => validatePass(covered, [C(2, 7)]), 'cannot pass over a covered battle');
});

test('validatePickup throws only on an empty table', () => {
  assert.throws(() => validatePickup(mkGame([6, 6], [], { selfSeat: 1 })), /cannot pickup/i, 'nothing to pick up');
  assert.doesNotThrow(
    () => validatePickup(mkGame([6, 6], [{ attack: C(0, 7), defense: null }], { selfSeat: 1 })),
    'a defender can scoop a non-empty table',
  );
});

// The Take button's enable state. The board used to spell this out itself - "I
// am the defender and the table is not empty" - which is handle_pickup's rule
// written a second time, and written short: it had no notion of a game that has
// already ended, so a finished board still offered Take.
test('canPickup is the defender, a non-empty table, and a game still playing', () => {
  const table = [{ attack: C(0, 7), defense: null }];
  assert.equal(canPickup(mkGame([6, 6], table, { selfSeat: 1 })), true, 'the defender may scoop a non-empty table');
  assert.equal(canPickup(mkGame([6, 6], [], { selfSeat: 1 })), false, 'an empty table has nothing to take');
  assert.equal(canPickup(mkGame([6, 6], table, { selfSeat: 0 })), false, 'an attacker never takes');

  const over = mkGame([6, 6], table, { selfSeat: 1 });
  over.status = GAME_STATUS.GAME_OVER;
  assert.equal(canPickup(over), false, 'a finished game offers nothing - the clause the hand-written gate lacked');
});

test('validateCover throws on an empty table and on an illegal cover, else passes', () => {
  assert.throws(
    () => validateCover(mkGame([6, 6], [], { selfSeat: 1, selfHand: [C(0, 9)] }), [C(0, 9)], [C(0, 7)]),
    'no table -> nothing to cover',
  );

  const g = mkGame([6, 6], [{ attack: C(0, 7), defense: null }], { selfSeat: 1, selfHand: [C(0, 9), C(0, 6)] });
  assert.doesNotThrow(() => validateCover(g, [C(0, 9)], [C(0, 7)]), '9♠ legally covers 7♠');
  assert.throws(() => validateCover(g, [C(0, 6)], [C(0, 7)]), '6♠ cannot cover 7♠');
});
