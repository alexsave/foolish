// Client-side move gates (src/utils/gameValidation.ts). The rule gates now
// delegate to the kernel (the client slot's client_validate) - e2e/client_guards
// fuzzes them against the authoritative server kernel across thousands of states. This file keeps
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
  canAttack, canCoverCards, canPickup, coverGesture, validateAttack, validatePass, validatePickup, validateCover,
} from '../src/utils/gameValidation.ts';
import type { TableView, ViewCard as Card } from '../sdk/ts/table/client_table.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { boardFixture, fixtureView, type BoardSpec } from './helpers/table_mem.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const C = (suit: number, value: number): Card => ({ suit, value });

// Diamonds (3) trump; seat 0 attacks / seat 1 defends. `self` is the acting
// seat with a real hand; every other seat holds `handLens` cards nobody names
// (the defender `defenderHand`). The board is sealed by the kernel and read
// from the viewer's envelope, as the client reads it.
const mkGame = (
  handLens: number[],
  table: NonNullable<BoardSpec['table']>,
  opts: { defenderHand?: number; selfSeat?: number; selfHand?: Card[]; status?: number } = {},
): TableView => {
  const { defenderHand = 6, selfSeat = 0, selfHand = [], status } = opts;
  const hands = handLens.map((n, i) => (i === selfSeat ? selfHand : i === 1 ? defenderHand : n));
  return fixtureView(boardFixture({ hands, table, powerSuit: 3, attacker: 0, defender: 1, status }), selfSeat);
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

// ---- the Cover button (client_play, legal.h play_*) -------------------------
//
// This used to be an affordance of its own: canCoverCards asked legal.c's
// unambiguous_cover "do these cards cover these attacks in exactly one way",
// a pure card-arithmetic question that read neither the seat nor the hand.
// It is the kernel's gesture rule now - the board's own menu, for the viewer's
// own seat - and two things change with it. A selection with SEVERAL legal
// targets is now offered rather than withheld, because the kernel aims the
// button (play_best_cover_target: the highest attack it beats, trumps
// outranking everything, ties to the leftmost) instead of refusing to choose.
// And the viewer has to be the seat that could actually make the move, holding
// the cards, which the old answer never checked.
//
// The viewer is seat 1, the defender, holding what it plays.

const defending = (table: NonNullable<BoardSpec['table']>, hand: Card[]): TableView =>
  mkGame([6, 6], table, { selfSeat: 1, selfHand: hand });

test('the Cover button aims at the highest attack it beats, and is offered whenever it has one', () => {
  const two = defending([{ attack: C(0, 7), defense: null }, { attack: C(0, 8), defense: null }], [C(0, 9)]);
  assert.equal(canCoverCards(two, [C(0, 9)]), true, 'a 9♠ over both 7♠ and 8♠ is offered, not withheld');
  assert.deepEqual(coverGesture(two, [C(0, 9)])!.attackCards.map((c) => ({ suit: c.suit, value: c.value })),
    [C(0, 8)], 'and it aims at the 8♠, the higher of the two');

  const one = defending([{ attack: C(0, 7), defense: null }], [C(0, 9), C(0, 6)]);
  assert.equal(canCoverCards(one, [C(0, 9)]), true, 'exactly one legal target -> offered');
  assert.equal(canCoverCards(one, [C(0, 6)]), false, 'a card that cannot cover -> not offered');
  assert.equal(canCoverCards(one, []), false, 'no selection -> false');
});

test('the Cover button is the viewer\'s own move: the wrong seat, or a card not held, is no move', () => {
  const table = [{ attack: C(0, 7), defense: null }];
  const attacker = mkGame([6, 6], table, { selfSeat: 0, selfHand: [C(0, 9)] });
  assert.equal(canCoverCards(attacker, [C(0, 9)]), false, 'an attacker covers nothing');
  const empty = mkGame([6, 6], table, { selfSeat: 1, selfHand: [C(0, 6)] });
  assert.equal(canCoverCards(empty, [C(0, 9)]), false, 'a card the defender does not hold is not playable');
});

test('a multi-card cover is one move over several battles', () => {
  const g = defending([{ attack: C(0, 7), defense: null }, { attack: C(2, 8), defense: null }], [C(0, 9), C(2, 9)]);
  const move = coverGesture(g, [C(0, 9), C(2, 9)]);
  assert.ok(move, 'each cover fits an attack, so the button is live');
  assert.equal(move!.cards.length, 2, 'and it lays both cards');

  // Two trumps over three plain attacks: several pairings exist. The old
  // affordance withheld the button; the kernel picks the strongest target.
  const many = defending([
    { attack: C(0, 7), defense: null }, { attack: C(0, 8), defense: null }, { attack: C(0, 9), defense: null },
  ], [C(3, 10), C(3, 11)]);
  assert.equal(canCoverCards(many, [C(3, 10), C(3, 11)]), true, 'two trumps over three attacks is offered');
  assert.ok(coverGesture(many, [C(3, 10), C(3, 11)])!.attackCards.some((c) => c.suit === 0 && c.value === 9),
    'and it covers the 9♠, the highest of the three');
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

  const over = mkGame([6, 6], table, { selfSeat: 1, status: L.GAME_STATUS_GAME_OVER });
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
