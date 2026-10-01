// Client-side move gates (src/utils/gameValidation.ts). Which action buttons a
// selection offers is the kernel's play_pills (boardPills), and the
// optimistic pre-check is the kernel's client_validate (validateActionWire);
// e2e/client_guards fuzzes both against the authoritative server kernel across
// thousands of states. This file keeps hand-picked concrete cases (readable
// regressions).
//
// The gates judge the LOCAL player's own move, so `self` must be the acting
// seat and actually hold the cards it plays (the kernel checks membership -
// the old hand-rolled TS gates did not).
//
// Pure client logic - needs no Postgres and no DOM.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { boardPills, coverGesture, validateActionWire } from '../src/utils/gameValidation.ts';
import { encodeAction, type AwireMove } from '../sdk/ts/wire/awire.ts';
import type { TableView, ViewCard as Card } from '../sdk/ts/table/client_table.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import * as V from '../sdk/ts/gen/view_layout.bots.ts';
import { boardFixture, fixtureView, type BoardSpec } from './helpers/table_mem.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const C = (suit: number, value: number): Card => ({ suit, value });

// Whether the board shows the button `pill` for the selection `cards`.
const shows = (view: TableView, cards: Card[], pill: number): boolean => (boardPills(view, cards) & pill) !== 0;
// The optimistic pre-check's verdict on a move: whether it lets the move through.
const passes = (view: TableView, move: AwireMove): boolean => {
  try { validateActionWire(view, encodeAction(move)); return true; } catch { return false; }
};

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

// ---- the Attack button -------------------------------------------------------

test('Attack: first attack requires held, same-value cards within defender capacity', () => {
  const hand = [C(0, 5), C(1, 5), C(1, 6)];
  const g = mkGame([6, 6], [], { selfHand: hand });
  assert.equal(shows(g, [], V.PLAY_PILL_ATTACK), false, 'no cards -> false');
  assert.equal(shows(g, [C(0, 5), C(1, 5)], V.PLAY_PILL_ATTACK), true, 'a held same-value pair opens');
  assert.equal(shows(g, [C(0, 5), C(1, 6)], V.PLAY_PILL_ATTACK), false, 'mixed values cannot open');
  assert.equal(shows(g, [C(2, 5)], V.PLAY_PILL_ATTACK), false, 'a card not in hand cannot be played');

  const tight = mkGame([6, 6], [], { defenderHand: 1, selfHand: hand });
  assert.equal(shows(tight, [C(0, 5), C(1, 5)], V.PLAY_PILL_ATTACK), false, 'cannot exceed defender capacity');
});

test('Attack: a follow-up attack must match a value already on the table', () => {
  const g = mkGame([6, 6], [{ attack: C(0, 7), defense: C(0, 9) }], { selfHand: [C(2, 7), C(2, 9), C(2, 8)] });
  assert.equal(shows(g, [C(2, 7)], V.PLAY_PILL_ATTACK), true, 'attack value present on the table (attack side)');
  assert.equal(shows(g, [C(2, 9)], V.PLAY_PILL_ATTACK), true, 'attack value present on the table (defense side)');
  assert.equal(shows(g, [C(2, 8)], V.PLAY_PILL_ATTACK), false, 'value not on the table');
});

// ---- the Cover button (client_play, legal.h play_*) -------------------------
//
// This used to be an affordance of its own: a canCoverCards asked legal.c's
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
  assert.equal(shows(two, [C(0, 9)], V.PLAY_PILL_COVER), true, 'a 9♠ over both 7♠ and 8♠ is offered, not withheld');
  assert.deepEqual(coverGesture(two, [C(0, 9)])!.attackCards.map((c) => ({ suit: c.suit, value: c.value })),
    [C(0, 8)], 'and it aims at the 8♠, the higher of the two');

  const one = defending([{ attack: C(0, 7), defense: null }], [C(0, 9), C(0, 6)]);
  assert.equal(shows(one, [C(0, 9)], V.PLAY_PILL_COVER), true, 'exactly one legal target -> offered');
  assert.equal(shows(one, [C(0, 6)], V.PLAY_PILL_COVER), false, 'a card that cannot cover -> not offered');
  assert.equal(shows(one, [], V.PLAY_PILL_COVER), false, 'no selection -> false');
});

test('the Cover button is the viewer\'s own move: the wrong seat, or a card not held, is no move', () => {
  const table = [{ attack: C(0, 7), defense: null }];
  const attacker = mkGame([6, 6], table, { selfSeat: 0, selfHand: [C(0, 9)] });
  assert.equal(shows(attacker, [C(0, 9)], V.PLAY_PILL_COVER), false, 'an attacker covers nothing');
  const empty = mkGame([6, 6], table, { selfSeat: 1, selfHand: [C(0, 6)] });
  assert.equal(shows(empty, [C(0, 9)], V.PLAY_PILL_COVER), false, 'a card the defender does not hold is not playable');
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
  assert.equal(shows(many, [C(3, 10), C(3, 11)], V.PLAY_PILL_COVER), true, 'two trumps over three attacks is offered');
  assert.ok(coverGesture(many, [C(3, 10), C(3, 11)])!.attackCards.some((c) => c.suit === 0 && c.value === 9),
    'and it covers the 9♠, the highest of the three');
});

// ---- the optimistic pre-check (validateActionWire) ----------------------------

test('the pre-check refuses an attack over capacity or off the table, else lets it through', () => {
  const roomy = mkGame([6, 6], [{ attack: C(0, 7), defense: null }], { selfHand: [C(2, 7), C(2, 8)] });
  assert.ok(passes(roomy, { kind: 'attack', cards: [C(2, 7)] }), 'held, value on table, room to defend');

  const tight = mkGame([6, 6], [{ attack: C(0, 7), defense: null }], { defenderHand: 1, selfHand: [C(2, 7)] });
  assert.ok(!passes(tight, { kind: 'attack', cards: [C(2, 7)] }), 'uncovered + new > defender hand');

  assert.ok(!passes(roomy, { kind: 'attack', cards: [C(2, 8)] }), 'off-table value rejected');
});

test('the pre-check refuses a mixed or un-passable pass, else lets it through', () => {
  const g = mkGame([6, 6], [{ attack: C(0, 7), defense: null }], { selfSeat: 1, selfHand: [C(2, 7), C(1, 8)] });
  assert.ok(passes(g, { kind: 'pass', cards: [C(2, 7)] }), 'defender holds a 7, single uncovered 7 on the table');
  assert.ok(!passes(g, { kind: 'pass', cards: [C(2, 7), C(1, 8)] }), 'mixed pass values');

  const covered = mkGame([6, 6], [{ attack: C(0, 7), defense: C(0, 9) }], { selfSeat: 1, selfHand: [C(2, 7)] });
  assert.ok(!passes(covered, { kind: 'pass', cards: [C(2, 7)] }), 'cannot pass over a covered battle');
});

test('the pre-check refuses a pickup only over an empty table', () => {
  assert.ok(!passes(mkGame([6, 6], [], { selfSeat: 1 }), { kind: 'pickup' }), 'nothing to pick up');
  assert.ok(passes(mkGame([6, 6], [{ attack: C(0, 7), defense: null }], { selfSeat: 1 }), { kind: 'pickup' }),
    'a defender can scoop a non-empty table');
});

test('the pre-check refuses a cover over an empty table or one that does not beat, else lets it through', () => {
  const cover = (covers: Card[], attacks: Card[]): AwireMove => ({ kind: 'cover', cards: covers, attack_cards: attacks });
  assert.ok(!passes(mkGame([6, 6], [], { selfSeat: 1, selfHand: [C(0, 9)] }), cover([C(0, 9)], [C(0, 7)])),
    'no table -> nothing to cover');

  const g = mkGame([6, 6], [{ attack: C(0, 7), defense: null }], { selfSeat: 1, selfHand: [C(0, 9), C(0, 6)] });
  assert.ok(passes(g, cover([C(0, 9)], [C(0, 7)])), '9♠ legally covers 7♠');
  assert.ok(!passes(g, cover([C(0, 6)], [C(0, 7)])), '6♠ cannot cover 7♠');
});

// ---- Take and Good: the two buttons a selection takes away ----------------------
//
// The Take button's enable state. The board used to spell this out itself - "I
// am the defender and the table is not empty" - which is handle_pickup's rule
// written a second time, and written short: it had no notion of a game that has
// already ended, so a finished board still offered Take. And neither Take nor
// Good knew about the selection, so a quick tap aimed at Pass or at a throw-in
// could land on a button that threw the selected cards away (legal.h
// play_pills, the rule the iMessage board draws by).
test('Take is the defender, a non-empty table, a game still playing, and no card selected', () => {
  const table = [{ attack: C(0, 7), defense: null }];
  const PICKUP = V.PLAY_PILL_PICKUP;
  assert.ok(shows(mkGame([6, 6], table, { selfSeat: 1 }), [], PICKUP), 'the defender may scoop a non-empty table');
  assert.ok(!shows(mkGame([6, 6], [], { selfSeat: 1 }), [], PICKUP), 'an empty table has nothing to take');
  assert.ok(!shows(mkGame([6, 6], table, { selfSeat: 0 }), [], PICKUP), 'an attacker never takes');

  const over = mkGame([6, 6], table, { selfSeat: 1, status: L.GAME_STATUS_GAME_OVER });
  assert.ok(!shows(over, [], PICKUP), 'a finished game offers nothing - the clause the hand-written gate lacked');

  const covered = mkGame([6, 6], [{ attack: C(0, 7), defense: C(0, 9) }], { selfSeat: 1, selfHand: [C(0, 6)] });
  assert.ok(shows(covered, [], PICKUP), 'a covered table may still be taken');

  const holding = mkGame([6, 6], table, { selfSeat: 1, selfHand: [C(0, 9), C(2, 7)] });
  assert.equal(boardPills(holding, [C(0, 9)]), V.PLAY_PILL_COVER, 'a covering card selected: Cover, and Take is gone');
  assert.equal(boardPills(holding, [C(2, 7)]), V.PLAY_PILL_PASS, 'a passing card selected: Pass, and Take is gone');
});

test('Good is offered over a covered table, and not while a throw-in is selected', () => {
  const g = mkGame([6, 6], [{ attack: C(0, 7), defense: C(0, 9) }], { selfHand: [C(2, 7), C(2, 8)] });
  assert.equal(boardPills(g, []), V.PLAY_PILL_GOOD, 'nothing selected over a covered table: Good alone');
  assert.equal(boardPills(g, [C(2, 7)]), V.PLAY_PILL_ATTACK, 'a throw-in selected: Attack, and Good is gone');
  assert.equal(boardPills(g, [C(2, 8)]), 0, 'a card that cannot be thrown in: no button at all');
});
