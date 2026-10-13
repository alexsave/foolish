/* =============================================================================
 * THE PLAN'S QUESTION, THROUGH THE BRIDGE
 * =============================================================================
 * `animBuildPlan` hands the kernel each step as what moved (ClientPlanStep,
 * through the generated writer) and the board it left (a TableView, through
 * its own), and the kernel reads the counts, the row and the masking off the
 * boards (c/src/client_table.h client_plan_step, client_plan). What is left in
 * TypeScript is an absent seat, an absent place and an absent board each
 * becoming the value the kernel uses for "none".
 *
 * The rules are pinned natively (c/tests/tests.c test_plan_ask_*,
 * test_client_plan_*). e2e/anim_plan_frames.test.ts pins the plan a stream WITH
 * boards gets. This file pins the rest of the door: the final board, which only
 * a stream with no board of its own reads; the row; the mask; the "none"
 * values; and that a question the kernel refuses is thrown, not answered with
 * whatever plan was there before.
 *
 * Pure kernel/wasm test - needs no Postgres.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { animBuildPlan, ANIM_EVT, ANIM_LOC, type AnimPlanEventIn } from '../sdk/ts/wasm/bots.ts';
import { ANIM_LOC_NONE, ANIM_SEAT_NONE } from '../sdk/ts/gen/anim.bots.ts';
import { ANIM_TABLE_NONE } from '../sdk/ts/gen/kernel_consts.bots.ts';
import { CARD_NONE_SUIT, CARD_NONE_VALUE, GAME_STATUS_PLAYING, PLAYER_STATUS_IN, type TableView_Snap as TableView } from '../sdk/ts/gen/view_layout.bots.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

interface Card { suit: number; value: number }
const C = (suit: number, value: number): Card => ({ suit, value });
const id = (c: Card) => c.suit * 13 + (c.value - 1);
const NO_CARD: Card = { suit: CARD_NONE_SUIT, value: CARD_NONE_VALUE };
const HIDDEN = C(-1, -1);
const TRUMP = C(2, 12), SIX_D = C(3, 6), KING_D = C(3, 13);

// A board as the plan reads one: the stock, the discard, the trump while it is
// face up, a hand count per seat and the row, each battle an attack and its
// cover when it has one.
const board = (
    b: { deck: number; discard: number; flipped: Card | null; hand: number[] }, row: Card[][] = [],
): TableView => ({
    status: GAME_STATUS_PLAYING, powerSuit: 2, firstAttacker: 0, defender: 1, mySeat: -1, fool: -1,
    deckCount: b.deck, discardPileLength: b.discard, hasFlipped: b.flipped !== null, hasGoodTimestamp: false,
    passing: true, flipped: b.flipped ?? NO_CARD, goodMask: 0, version: 0,
    battles: row.map(([attack, defense]) => ({ attack, defense: defense ?? NO_CARD })),
    seats: b.hand.map((handCount) => ({ status: PLAYER_STATUS_IN, handCount, awaitingAttack: false, isAi: false, id: '', name: '' })),
    myHand: [], elimination: [], rated: false, ratingDelta: new Array(8).fill(0), ratingBefore: new Array(8).fill(0), gameId: 'g', title: '',
});

// A pickup of two cards that carries no board: the only kind of stream that
// reads the final board, so the only one that shows it crossed.
const boardlessPickup = (): AnimPlanEventIn[] => [
    { type: ANIM_EVT.pickup, seat: 1, from: ANIM_LOC.table, to: ANIM_LOC.hand, cards: [C(0, 9), C(2, 9)] },
];

test('the final board crosses: a boardless stream is frozen back from it', () => {
    const p = animBuildPlan(boardlessPickup(), board({ deck: 7, discard: 2, flipped: TRUMP, hand: [3, 9, 4] }));
    assert.equal(p.pre.deck, 7);
    assert.equal(p.pre.discard, 2);
    assert.equal(p.pre.nPlayers, 3);
    // Seat 1's nine with the two it picked up taken back out; the others as given.
    assert.deepEqual(p.pre.hand, [3, 7, 4]);
    assert.deepEqual(p.pre.flipped, TRUMP);
});

test('with no final board the table is as wide as the widest board a step carried', () => {
    const out: AnimPlanEventIn = { type: ANIM_EVT.out, board: board({ deck: 0, discard: 0, flipped: null, hand: [1, 0, 2] }) };
    assert.equal(animBuildPlan([...boardlessPickup(), out], undefined).pre.nPlayers, 3);
    // ...and a stream that carries no board either has no table at all.
    assert.throws(() => animBuildPlan(boardlessPickup(), undefined), /anim_build_plan error/);
});

test('the row a step\'s board holds crosses with it, and an empty table is no row', () => {
    const pass = (row: Card[][]): AnimPlanEventIn => ({
        type: ANIM_EVT.attack_pass, seat: 0, from: ANIM_LOC.hand, to: ANIM_LOC.table,
        cards: [KING_D],
        board: board({ deck: 12, discard: 0, flipped: TRUMP, hand: [5, 6] }, row),
    });
    const final = board({ deck: 12, discard: 0, flipped: TRUMP, hand: [5, 6] });
    const withRow = animBuildPlan([pass([[SIX_D], [KING_D]])], final);
    // The row before the pass: the pile that was already down, bare.
    assert.equal(withRow.pre.nBattles, 1);
    assert.deepEqual(withRow.pre.battles.slice(0, 2), [id(SIX_D), ANIM_TABLE_NONE]);
    assert.equal(withRow.pre.paired, true);

    assert.equal(animBuildPlan([pass([])], final).pre.nBattles, 0);
});

test('a board with no trump face up is CARD_NONE to the kernel, and a hidden trump is not', () => {
    // A draw whose board before it had a trump. With no trump face up on its own
    // board the slot is empty, so this is the draw that dealt the trump out...
    const lead: AnimPlanEventIn = {
        type: ANIM_EVT.cards_to_trash, from: ANIM_LOC.table, to: ANIM_LOC.discard, cards: [SIX_D],
        board: board({ deck: 1, discard: 1, flipped: TRUMP, hand: [5, 5] }),
    };
    const draw = (flipped: Card | null): AnimPlanEventIn => ({
        type: ANIM_EVT.refill, seat: 0, from: ANIM_LOC.deck, to: ANIM_LOC.hand, cards: [HIDDEN, HIDDEN],
        board: board({ deck: 0, discard: 1, flipped, hand: [7, 5] }),
    });
    const final = board({ deck: 0, discard: 1, flipped: null, hand: [7, 5] });
    assert.deepEqual(animBuildPlan([lead, draw(null)], final).steps[1].trumpOut, TRUMP);
    // ...and with a HIDDEN card there the slot still holds a card, so it is not.
    assert.equal(animBuildPlan([lead, draw(HIDDEN)], final).steps[1].trumpOut, null);
});

test('a step of nothing but backs is masked, and one that names a card is not', () => {
    const draw = (cards: Card[]): AnimPlanEventIn => ({
        type: ANIM_EVT.refill, seat: 1, from: ANIM_LOC.deck, to: ANIM_LOC.hand, cards,
        board: board({ deck: 2, discard: 0, flipped: TRUMP, hand: [6, 6] }),
    });
    const final = board({ deck: 2, discard: 0, flipped: TRUMP, hand: [6, 6] });
    assert.deepEqual(animBuildPlan([draw([SIX_D, KING_D])], final).veilIds, [id(SIX_D), id(KING_D)]);
    assert.deepEqual(animBuildPlan([draw([HIDDEN, HIDDEN])], final).veilIds, []);
    // One named card is enough: the step is not a masked one, and that card is veiled.
    assert.deepEqual(animBuildPlan([draw([SIX_D, HIDDEN])], final).veilIds, [id(SIX_D)]);
});

test('an absent seat and an absent place cross as the kernel\'s own "none"', () => {
    const p = animBuildPlan([{ type: ANIM_EVT.out }, { type: ANIM_EVT.out, seat: -1 }],
        board({ deck: 0, discard: 0, flipped: null, hand: [1, 0] }));
    for (const s of p.steps) {
        assert.equal(s.seat, ANIM_SEAT_NONE);
        assert.equal(s.from, ANIM_LOC_NONE);
        assert.equal(s.to, ANIM_LOC_NONE);
    }
});

test('a question the kernel refuses is thrown, and so is one that does not fit the ask', () => {
    const final = board({ deck: 0, discard: 0, flipped: null, hand: [6, 6] });
    const out: AnimPlanEventIn = { type: ANIM_EVT.out };
    // The kernel's own refusals: a table of one, a 129th step, and a row of 53 battles.
    assert.throws(() => animBuildPlan([out], board({ deck: 0, discard: 0, flipped: null, hand: [6] })), /anim_build_plan error/);
    assert.throws(() => animBuildPlan(new Array<AnimPlanEventIn>(129).fill(out), final), /anim_build_plan error/);
    const wide = board({ deck: 0, discard: 0, flipped: null, hand: [6, 6] }, new Array<Card[]>(53).fill([SIX_D]));
    assert.throws(() => animBuildPlan([{ ...out, board: wide }], final), /anim_build_plan error/);
    // The writer's: a 53rd card never reaches the kernel.
    assert.throws(() => animBuildPlan([{ ...out, cards: new Array<Card>(53).fill(SIX_D) }], final), RangeError);
    // ...and the widest stream that does fit is planned, straight after the refusals.
    assert.equal(animBuildPlan(new Array<AnimPlanEventIn>(128).fill(out), final).nSteps, 128);
});
