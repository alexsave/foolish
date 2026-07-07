// Client-side move gates (src/utils/gameValidation.ts) drive the UI's
// button-enable / drag-drop logic and the optimistic pre-checks. canPass /
// nextDefenderIndex are policed by the parity suites; this covers the rest:
// canAttack, the permutation-based canCoverCards (single + multi-card
// ambiguity), and the four throwing validators.
//
// Pure client logic — needs no Postgres and no DOM.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  canAttack, canCoverCards, validateAttack, validatePass, validatePickup, validateCover,
} from '../src/utils/gameValidation.ts';
import {
  PersonalGame, PublicPlayer, Card, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
} from '../supabase/functions/_shared/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const C = (suit: number, value: number): Card => ({ suit, value });
const P = (i: number, handLen: number): PublicPlayer => ({
  name: `P${i}`, player_id: `p${i}`, status: PLAYER_STATUS.IN, hand_length: handLen, is_ai: false,
});

// PersonalGame with diamonds (3) trump; seat 0 attacks, seat 1 defends.
const mkGame = (handLens: number[], table: PersonalGame['table_battles'], defenderHand = 6): PersonalGame => {
  const players = handLens.map((n, i) => P(i, i === 1 ? defenderHand : n));
  return {
    id: 'g', name: 'g', deck_length: 0, discard_pile_length: 0, flipped: null,
    players, status: GAME_STATUS.PLAYING, power_suit: 3, first_attacker: 0, defender: 1,
    table_battles: table, elimination_order: [], good_timestamp: null, good_players: [],
    self: { player_id: 'p0', name: 'P0', status: PLAYER_STATUS.IN, is_ai: false, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: STRATEGY_KEY.HUMAN },
  };
};

// ---- canAttack --------------------------------------------------------------

test('canAttack: first attack requires one shared value within defender capacity', () => {
  const empty = mkGame([6, 6], [], /*defenderHand*/ 6);
  assert.equal(canAttack(empty, []), false, 'no cards -> false');
  assert.equal(canAttack(empty, [C(0, 5), C(1, 5)]), true, 'a same-value pair opens');
  assert.equal(canAttack(empty, [C(0, 5), C(1, 6)]), false, 'mixed values cannot open');

  const tightDefender = mkGame([6, 6], [], /*defenderHand*/ 1);
  assert.equal(canAttack(tightDefender, [C(0, 5), C(1, 5)]), false, 'cannot exceed defender capacity');
});

test('canAttack: a follow-up attack must match a value already on the table', () => {
  const g = mkGame([6, 6], [{ attack: C(0, 7), defense: C(0, 9) }], /*defenderHand*/ 6);
  assert.equal(canAttack(g, [C(2, 7)]), true, 'attack value present on the table (attack side)');
  assert.equal(canAttack(g, [C(2, 9)]), true, 'attack value present on the table (defense side)');
  assert.equal(canAttack(g, [C(2, 8)]), false, 'value not on the table');
});

// ---- canCoverCards ----------------------------------------------------------

test('canCoverCards: single card is offered only when its target is unambiguous', () => {
  // 7♠ and 8♠ uncovered; a non-trump 9♠ covers only 7♠? No — 9♠ covers BOTH
  // (higher same suit). So single-card 9♠ is ambiguous -> hidden.
  const two = mkGame([6, 6], [{ attack: C(0, 7), defense: null }, { attack: C(0, 8), defense: null }]);
  assert.equal(canCoverCards(two, [C(0, 9)]), false, 'a 9♠ covering both 7♠ and 8♠ is ambiguous');

  // Only 7♠ uncovered -> 9♠ has exactly one target -> offered.
  const one = mkGame([6, 6], [{ attack: C(0, 7), defense: null }]);
  assert.equal(canCoverCards(one, [C(0, 9)]), true, 'exactly one legal target -> offered');
  assert.equal(canCoverCards(one, [C(0, 6)]), false, 'a card that cannot cover -> not offered');
  assert.equal(canCoverCards(one, []), false, 'no selection -> false');

  const covered = mkGame([6, 6], [{ attack: C(0, 7), defense: C(0, 9) }]);
  assert.equal(canCoverCards(covered, [C(0, 10)]), false, 'nothing uncovered -> false');
});

test('canCoverCards: multi-card cover is offered only when the mapping is unambiguous', () => {
  // 7♠ + 8♣ uncovered; 9♠ covers only 7♠, 9♣ covers only 8♣ -> one mapping.
  const unambiguous = mkGame([6, 6], [{ attack: C(0, 7), defense: null }, { attack: C(2, 8), defense: null }]);
  assert.equal(canCoverCards(unambiguous, [C(0, 9), C(2, 9)]), true, 'each cover fits exactly one attack');

  // Two trumps over THREE uncovered attacks -> covers could target different
  // pairs -> ambiguous which two get covered -> hidden.
  const ambiguous = mkGame([6, 6], [
    { attack: C(0, 7), defense: null }, { attack: C(0, 8), defense: null }, { attack: C(0, 9), defense: null },
  ]);
  assert.equal(canCoverCards(ambiguous, [C(3, 10), C(3, 11)]), false, 'two trumps over three attacks is ambiguous');

  // Covers that fit nothing -> false.
  const nofit = mkGame([6, 6], [{ attack: C(0, 7), defense: null }, { attack: C(2, 8), defense: null }]);
  assert.equal(canCoverCards(nofit, [C(0, 3), C(2, 3)]), false, 'covers that cannot beat their attacks');
});

// ---- throwing validators ----------------------------------------------------

test('validateAttack throws on over-capacity and off-table values, else passes', () => {
  const roomy = mkGame([6, 6], [{ attack: C(0, 7), defense: null }], /*defenderHand*/ 6);
  assert.doesNotThrow(() => validateAttack(roomy, [C(2, 7)]), 'value on table, room to defend');

  const tight = mkGame([6, 6], [{ attack: C(0, 7), defense: null }], /*defenderHand*/ 1);
  assert.throws(() => validateAttack(tight, [C(2, 7)]), /no room/i, 'uncovered + new > defender hand');

  assert.throws(() => validateAttack(roomy, [C(2, 8)]), /not on the table/i, 'off-table value rejected');
});

test('validatePass throws on mixed values and un-passable tables, else passes', () => {
  const g = mkGame([6, 6], [{ attack: C(0, 7), defense: null }]);
  assert.doesNotThrow(() => validatePass(g, [C(2, 7)]), 'single uncovered 7, passing a 7');
  assert.throws(() => validatePass(g, [C(2, 7), C(1, 8)]), /not the same/i, 'mixed pass values');

  const coveredTable = mkGame([6, 6], [{ attack: C(0, 7), defense: C(0, 9) }]);
  assert.throws(() => validatePass(coveredTable, [C(2, 7)]), /cannot pass/i, 'cannot pass over a covered battle');
});

test('validatePickup throws only on an empty table', () => {
  assert.throws(() => validatePickup(mkGame([6, 6], [])), /cannot pickup/i, 'nothing to pick up');
  assert.doesNotThrow(() => validatePickup(mkGame([6, 6], [{ attack: C(0, 7), defense: null }])), 'a non-empty table can be scooped');
});

test('validateCover throws on an empty table and on an illegal cover, else passes', () => {
  assert.throws(() => validateCover(mkGame([6, 6], []), [C(0, 9)], [C(0, 7)]), /cannot cover/i, 'no table');

  const g = mkGame([6, 6], [{ attack: C(0, 7), defense: null }]);
  assert.doesNotThrow(() => validateCover(g, [C(0, 9)], [C(0, 7)]), '9♠ legally covers 7♠');
  assert.throws(() => validateCover(g, [C(0, 6)], [C(0, 7)]), /does not match/i, '6♠ cannot cover 7♠');
});
