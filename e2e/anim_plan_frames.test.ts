/* =============================================================================
 * THE PLAN, RE-ASKED: the web's frame loop against the kernel's
 * =============================================================================
 * docs/C_GAME_SHAPE_MIGRATION.md Phase 9 step 3. The web used to keep "where is
 * this sequence now" nowhere at all: a chain of setTimeouts, one per step, each
 * committing a board and arming the next, so a push landing mid-flight could
 * only be applied by editing the queue the chain was walking.
 *
 * `anim_plan_at` replaces that with a value: given the plan and a clock the host
 * owns, the answer is which step is flying, how far into it, how many have
 * landed (the board to commit), what the badges read, what is still veiled, and
 * when the answer next changes. This file pins the WASM BRIDGE to it - the door
 * the browser reaches that C through - because a rule that is right in C and
 * wrong in the marshal is wrong on screen.
 *
 * The C rules themselves are pinned natively (c/tests/anim_plan_test.c section
 * 8); what is new here is the byte layout of the inputs, the struct reads
 * through the generated accessors (sdk/ts/gen/anim.bots.ts), and the sentinels
 * that model "no seat", "no board" and "no good mask".
 *
 * Pure kernel/wasm test - needs no Postgres.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    animBuildPlan, animPlanAt, animBuildBeats, ANIM_EVT, ANIM_LOC,
    ANIM_STEP_NONE, ANIM_NEVER,
    type AnimPlanEventIn, type AnimBeatEventIn,
} from '../sdk/ts/wasm/bots.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

interface Card { suit: number; value: number }
const C = (suit: number, value: number): Card => ({ suit, value });
const id = (c: Card) => c.suit * 13 + (c.value - 1);
const NONE = 0xfe;                      // ANIM_TABLE_NONE: an uncovered battle

// The timing policy, as the kernel states it (anim_plan.h ANIM_TIME_MS / ANIM_GAP_MS).
const TIME = 500, GAP = 25;

const SIX_S = C(0, 5), SEVEN_S = C(0, 6), TRUMP = C(2, 12);

// A two-step bout: I lay 6s, the defender covers with 7s. Both events carry
// their own board, which is what makes the count-freeze derivable at all.
const bout = (): AnimPlanEventIn[] => [
    {
        type: ANIM_EVT.attack_pass, seat: 0, from: ANIM_LOC.hand, to: ANIM_LOC.table,
        cards: [SIX_S],
        counts: { deck: 4, discard: 0, flipped: TRUMP, hand: [5, 6] },
        battles: [id(SIX_S), NONE],
    },
    {
        type: ANIM_EVT.cover, seat: 1, from: ANIM_LOC.hand, to: ANIM_LOC.table,
        cards: [SEVEN_S],
        counts: { deck: 4, discard: 0, flipped: TRUMP, hand: [5, 5] },
        battles: [id(SIX_S), id(SEVEN_S)],
    },
];
const FINAL = { deck: 4, discard: 0, flipped: TRUMP as Card | null, hand: [5, 5] };

const plan = () => animBuildPlan(bout(), 2, FINAL);

test('a step lasts the kernel\'s time and the next one waits its gap', () => {
    const p = plan();
    assert.equal(p.nSteps, 2);
    assert.equal(p.steps[0].startMs, 0);
    assert.equal(p.steps[0].durationMs, TIME);
    assert.equal(p.steps[1].startMs, TIME + GAP, 'step 1 opens after the gap the web ran at zero');
    assert.equal(p.totalMs, 2 * TIME + GAP);
});

test('the freeze is the board BEFORE the sequence, row included', () => {
    const p = plan();
    // One undo off event 0's own board: my card comes back out of the row and
    // into my hand, and nothing else moves.
    assert.equal(p.pre.deck, 4);
    assert.equal(p.pre.discard, 0);
    assert.deepEqual(p.pre.hand, [6, 6]);
    assert.equal(p.pre.nBattles, 0, 'the row before the attack holds nothing');
    // The stock's other half: the trump lying under the deck. It is the freeze's
    // half that a deck count alone cannot carry, and the badge counts both.
    assert.deepEqual(p.pre.flipped, TRUMP);
});

test('both laid cards are veiled, and each step names the one it reveals', () => {
    const p = plan();
    assert.deepEqual([...p.veilIds].sort((a, b) => a - b), [id(SIX_S), id(SEVEN_S)].sort((a, b) => a - b));
    assert.equal(p.steps[0].reveals, 1n << BigInt(id(SIX_S)));
    assert.equal(p.steps[1].reveals, 1n << BigInt(id(SEVEN_S)));
});

test('frame zero: nothing has landed, the badges are frozen and both cards are veiled', () => {
    plan();
    const f = animPlanAt(0);
    assert.equal(f.step, 0);
    assert.equal(f.elapsedMs, 0);
    assert.equal(f.landed, 0, 'the board to commit is the freeze, not step 0\'s');
    assert.equal(f.nextMs, TIME);
    assert.equal(f.done, false);
    assert.deepEqual(f.hand, [6, 6]);
    assert.equal(f.veiled, (1n << BigInt(id(SIX_S))) | (1n << BigInt(id(SEVEN_S))));
});

test('one millisecond before the landing, the step is still flying', () => {
    plan();
    const f = animPlanAt(TIME - 1);
    assert.equal(f.step, 0);
    assert.equal(f.elapsedMs, TIME - 1);
    assert.equal(f.landed, 0);
});

test('the landing commits step 0 and the gap has no step in it', () => {
    plan();
    const f = animPlanAt(TIME);
    assert.equal(f.step, ANIM_STEP_NONE, 'the gap is a step of nothing, not step 1 started early');
    assert.equal(f.landed, 1);
    assert.equal(f.nextMs, TIME + GAP);
    assert.deepEqual(f.hand, [5, 6], 'step 0\'s own board');
    assert.equal(f.veiled, 1n << BigInt(id(SEVEN_S)), 'the card that landed is no longer veiled');
});

test('the end: everything has landed and the answer will not change again', () => {
    const p = plan();
    const f = animPlanAt(p.totalMs);
    assert.equal(f.step, ANIM_STEP_NONE);
    assert.equal(f.landed, 2);
    assert.equal(f.done, true);
    assert.equal(f.nextMs, ANIM_NEVER);
    assert.equal(f.veiled, 0n);
    assert.deepEqual(f.hand, [5, 5]);
});

test('a deal out of the deck carries its in-flight counts, and the trump does not shrink the badge', () => {
    const deal: AnimPlanEventIn[] = [{
        type: ANIM_EVT.refill, seat: 0, from: ANIM_LOC.deck, to: ANIM_LOC.flipped,
        cards: [TRUMP],
        counts: { deck: 0, discard: 0, flipped: null, hand: [6, 6] },
        battles: [],
    }];
    animBuildPlan(deal, 2, { deck: 0, discard: 0, flipped: null, hand: [6, 6] });
    const f = animPlanAt(0);
    assert.equal(f.inFlightFromDeck, 1);
    assert.equal(f.inFlightToFlipped, 1);
});

test('two covers by one seat are ONE beat, and a lone attack is its own', () => {
    const events: AnimBeatEventIn[] = [
        { type: ANIM_EVT.attack_pass, seat: 0, cards: [SIX_S] },
        { type: ANIM_EVT.cover, seat: 1, cards: [SEVEN_S] },
        { type: ANIM_EVT.cover, seat: 1, cards: [C(1, 6)] },
    ];
    const b = animBuildBeats(events);
    assert.equal(b.beats.length, 2, 'the kernel spends one COVER event per card; one move is one beat');
    assert.equal(b.beats[0].nEvents, 1);
    assert.equal(b.beats[1].first, 1);
    assert.equal(b.beats[1].nEvents, 2);
    assert.equal(b.placedIds,
        (1n << BigInt(id(SIX_S))) | (1n << BigInt(id(SEVEN_S))) | (1n << BigInt(id(C(1, 6)))));
});

test('a step that carries no good mask says so, and does not read as eight good seats', () => {
    const b = animBuildBeats([{ type: ANIM_EVT.attack_pass, seat: 0, cards: [SIX_S] }]);
    assert.equal(b.firstGoodMask, -1, 'ANIM_NO_MASK, not a mask of every seat');
    assert.equal(b.beats[0].goodMask, -1);
    const withMask = animBuildBeats([{ type: ANIM_EVT.attack_pass, seat: 0, cards: [SIX_S], goodMask: 0xff }]);
    assert.equal(withMask.firstGoodMask, 0xff, 'eight good seats is a real mask and must survive the wire');
});

test('a seatless notice keeps its seat out of the answer', () => {
    const b = animBuildBeats([{ type: ANIM_EVT.magic_transition }]);
    assert.equal(b.beats[0].seat, -1, 'ANIM_SEAT_NONE, not seat 255');
});
