/* =============================================================================
 * THE PLAN'S QUESTION, THROUGH THE BRIDGE
 * =============================================================================
 * `animBuildPlan` used to pack its stream into the kernel's io buffer a byte at
 * a time, in a format only a C comment described. It now fills the kernel's own
 * AnimPlanAsk through the generated writer (sdk/ts/gen/view_layout.bots.ts), and
 * what is left in TypeScript is the shape change: an absent seat, an absent
 * place, an absent board and an absent card each become the value the kernel
 * uses for "none".
 *
 * The rule itself - that the ask's plan is anim_build_plan's plan - is pinned
 * natively (c/tests/tests.c test_plan_ask_*). e2e/anim_plan_frames.test.ts pins
 * the plan a stream WITH boards gets. This file pins the rest of the door: the
 * final board, which only a stream with no board of its own reads; the row; the
 * mask; the "none" values; and that a question the kernel refuses is thrown, not
 * answered with whatever plan was there before.
 *
 * Pure kernel/wasm test - needs no Postgres.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { animBuildPlan, ANIM_EVT, ANIM_LOC, type AnimPlanEventIn } from '../sdk/ts/wasm/bots.ts';
import { ANIM_LOC_NONE, ANIM_SEAT_NONE } from '../sdk/ts/gen/anim.bots.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

interface Card { suit: number; value: number }
const C = (suit: number, value: number): Card => ({ suit, value });
const id = (c: Card) => c.suit * 13 + (c.value - 1);
const NONE = 0xfe;                      // ANIM_TABLE_NONE: an uncovered battle
const HIDDEN = C(-1, -1);
const TRUMP = C(2, 12), SIX_D = C(3, 6), KING_D = C(3, 13);

// A pickup of two cards that carries no board: the only kind of stream that
// reads the final board, so the only one that shows it crossed.
const boardlessPickup = (): AnimPlanEventIn[] => [
    { type: ANIM_EVT.pickup, seat: 1, from: ANIM_LOC.table, to: ANIM_LOC.hand, cards: [C(0, 9), C(2, 9)] },
];

test('the final board crosses: a boardless stream is frozen back from it', () => {
    const p = animBuildPlan(boardlessPickup(), 3, { deck: 7, discard: 2, flipped: TRUMP, hand: [3, 9, 4] });
    assert.equal(p.pre.deck, 7);
    assert.equal(p.pre.discard, 2);
    assert.equal(p.pre.nPlayers, 3);
    // Seat 1's nine with the two it picked up taken back out; the others as given.
    assert.deepEqual(p.pre.hand, [3, 7, 4]);
    assert.deepEqual(p.pre.flipped, TRUMP);
});

test('a final hand shorter than the table reads as empty seats, not as a refusal', () => {
    const p = animBuildPlan(boardlessPickup(), 3, { deck: 7, discard: 2, flipped: null, hand: [3, 9] });
    assert.deepEqual(p.pre.hand, [3, 7, 0]);
});

test('the row a step carries crosses with it, and a step that carries none gets none', () => {
    const pass: AnimPlanEventIn = {
        type: ANIM_EVT.attack_pass, seat: 0, from: ANIM_LOC.hand, to: ANIM_LOC.table,
        cards: [KING_D],
        counts: { deck: 12, discard: 0, flipped: TRUMP, hand: [5, 6] },
        battles: [id(SIX_D), NONE, id(KING_D), NONE],
    };
    const final = { deck: 12, discard: 0, flipped: TRUMP, hand: [5, 6] };
    const withRow = animBuildPlan([pass], 2, final);
    // The row before the pass: the pile that was already down, bare.
    assert.equal(withRow.pre.nBattles, 1);
    assert.deepEqual(withRow.pre.battles.slice(0, 2), [id(SIX_D), NONE]);
    assert.equal(withRow.pre.paired, true);

    const { battles: _row, ...bare } = pass;
    assert.equal(animBuildPlan([bare], 2, final).pre.nBattles, 0);
});

test('no card is CARD_NONE to the kernel, and a hidden card is not', () => {
    // A draw whose board before it had a trump. With `flipped: null` on its own
    // board the slot is empty, so this is the draw that dealt the trump out…
    const lead: AnimPlanEventIn = {
        type: ANIM_EVT.cards_to_trash, from: ANIM_LOC.table, to: ANIM_LOC.discard, cards: [SIX_D],
        counts: { deck: 1, discard: 1, flipped: TRUMP, hand: [5, 5] },
    };
    const draw = (flipped: Card | null): AnimPlanEventIn => ({
        type: ANIM_EVT.refill, seat: 0, from: ANIM_LOC.deck, to: ANIM_LOC.hand, cards: [HIDDEN, HIDDEN],
        maskCards: true,
        counts: { deck: 0, discard: 1, flipped, hand: [7, 5] },
    });
    const final = { deck: 0, discard: 1, flipped: null, hand: [7, 5] };
    assert.deepEqual(animBuildPlan([lead, draw(null)], 2, final).steps[1].trumpOut, TRUMP);
    // …and with a HIDDEN card there the slot still holds a card, so it is not.
    assert.equal(animBuildPlan([lead, draw(HIDDEN)], 2, final).steps[1].trumpOut, null);
});

test('a masked step\'s cards stay out of the veil, by the flag and not by what they are', () => {
    // The cards are given identities on purpose. A masked draw is normally all
    // {-1,-1}, which names nothing with or without the flag, so only cards that
    // COULD be veiled show the flag crossed.
    const draw = (maskCards: boolean): AnimPlanEventIn => ({
        type: ANIM_EVT.refill, seat: 1, from: ANIM_LOC.deck, to: ANIM_LOC.hand,
        cards: [SIX_D, KING_D], maskCards,
        counts: { deck: 2, discard: 0, flipped: TRUMP, hand: [6, 6] },
    });
    const final = { deck: 2, discard: 0, flipped: TRUMP, hand: [6, 6] };
    assert.deepEqual(animBuildPlan([draw(false)], 2, final).veilIds, [id(SIX_D), id(KING_D)]);
    assert.deepEqual(animBuildPlan([draw(true)], 2, final).veilIds, []);
});

test('an absent seat and an absent place cross as the kernel\'s own "none"', () => {
    const p = animBuildPlan([{ type: ANIM_EVT.out }, { type: ANIM_EVT.out, seat: -1 }], 2,
        { deck: 0, discard: 0, flipped: null, hand: [1, 0] });
    for (const s of p.steps) {
        assert.equal(s.seat, ANIM_SEAT_NONE);
        assert.equal(s.from, ANIM_LOC_NONE);
        assert.equal(s.to, ANIM_LOC_NONE);
    }
});

test('a question the kernel refuses is thrown, and so is one that does not fit the ask', () => {
    const final = { deck: 0, discard: 0, flipped: null, hand: [6, 6] };
    const out: AnimPlanEventIn = { type: ANIM_EVT.out };
    // The kernel's own refusals: a table of one, and half a battle.
    assert.throws(() => animBuildPlan([out], 1, final), /anim_build_plan error/);
    assert.throws(() => animBuildPlan([{ ...out, battles: [id(SIX_D), NONE, id(KING_D)] }], 2, final),
        /anim_build_plan error/);
    // The writer's: more than the ask has room for never reaches the kernel -
    // a 129th event, a 53rd card, a 53rd battle.
    assert.throws(() => animBuildPlan(new Array<AnimPlanEventIn>(129).fill(out), 2, final), RangeError);
    assert.throws(() => animBuildPlan([{ ...out, cards: new Array<Card>(53).fill(SIX_D) }], 2, final), RangeError);
    assert.throws(() => animBuildPlan([{ ...out, battles: new Array<number>(106).fill(NONE) }], 2, final), RangeError);
    // …and the widest stream that does fit is planned.
    assert.equal(animBuildPlan(new Array<AnimPlanEventIn>(128).fill(out), 2, final).nSteps, 128);
});
