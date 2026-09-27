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
    animBuildPlan, animPlanAt, animBuildBeats, animReversalOrder,
    ANIM_EVT, ANIM_LOC, ANIM_STEP_NONE, ANIM_NEVER, ANIM_TIME_MS, ANIM_BOUT_END_HOLD_MS,
    ANIM_CONFLICT_REVERT, ANIM_CONFLICT_KEEP, ANIM_CONFLICT_CLEAR,
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

// ---- the plan is laid out in BEATS ------------------------------------------
// The defect this pins, measured in a browser (docs/WEB_ANIM_PARITY.md section
// 3): a bout-ending cover landed and the sweep took the whole table away 36ms
// later. The web plays the kernel's plan and schedules nothing, so the rest can
// only come from the plan's own start_ms - which means the plan has to know the
// beats. These check the BYTE LAYOUT of that answer through the wasm bridge;
// the C rules are pinned natively in c/tests/anim_plan_test.c section 8b.

// A last defence as the wire delivers one: the cover that emptied the
// defender's hand, then the discard that sweeps the table, then a refill.
const lastDefence = (): AnimPlanEventIn[] => [
    { type: ANIM_EVT.cover, seat: 1, from: ANIM_LOC.hand, to: ANIM_LOC.table, cards: [SEVEN_S] },
    { type: ANIM_EVT.discard, from: ANIM_LOC.table, to: ANIM_LOC.discard, cards: [SIX_S, SEVEN_S] },
    { type: ANIM_EVT.refill, seat: 0, from: ANIM_LOC.deck, to: ANIM_LOC.hand, cards: [TRUMP] },
];

test('the hold is the kernel\'s number, and it is three flights long', () => {
    assert.equal(ANIM_BOUT_END_HOLD_MS, ANIM_TIME_MS * 3,
        'iOS BoardFlight.boutEndHold is flightTime * 3; a bare 1500 in a host is a second timing policy');
    assert.equal(ANIM_BOUT_END_HOLD_MS, 1500, 'and at the shipping flight time that is the 1.5s the owner asked for');
});

test('a bout-ending cover rests before the sweep, and the rest is inside the sweep\'s start', () => {
    const p = animBuildPlan(lastDefence(), 2, FINAL);
    assert.equal(p.steps[0].startMs, 0);
    assert.equal(p.steps[0].holdMs, ANIM_BOUT_END_HOLD_MS, 'the cover carries the hold');
    assert.equal(p.steps[1].startMs, TIME + GAP + ANIM_BOUT_END_HOLD_MS,
        'the sweep waits the landing, the gap AND the hold');
    assert.equal(p.steps[1].holdMs, 0, 'and nothing else in the sequence rests');
    assert.equal(p.steps[2].startMs, 2 * (TIME + GAP) + ANIM_BOUT_END_HOLD_MS,
        'the refill behind it keeps its ordinary gap');
    assert.equal(p.totalMs, 2 * (TIME + GAP) + ANIM_BOUT_END_HOLD_MS + TIME);
});

test('sampled inside the hold, nothing is flying and the covered table is what shows', () => {
    const p = animBuildPlan(lastDefence(), 2, FINAL);
    const f = animPlanAt(TIME + 400);
    assert.equal(f.step, ANIM_STEP_NONE, 'a hold is a beat of NOTHING moving - that is what makes it a hold');
    assert.equal(f.landed, 1, 'the board on show is the one the cover landed on');
    assert.equal(f.nextMs, p.steps[1].startMs, 'and the next thing to happen is the sweep, a hold away');
});

test('a cover with the bout still open keeps the ordinary gap', () => {
    const p = animBuildPlan([
        lastDefence()[0],
        { type: ANIM_EVT.attack_pass, seat: 0, from: ANIM_LOC.hand, to: ANIM_LOC.table, cards: [SIX_S] },
    ], 2, FINAL);
    assert.equal(p.steps[0].holdMs, 0, 'the hold is a bout END, not a pause between any two moves');
    assert.equal(p.steps[1].startMs, TIME + GAP);
});

test('two covers by one seat open at the same instant and name one beat', () => {
    const p = animBuildPlan([
        { type: ANIM_EVT.cover, seat: 1, from: ANIM_LOC.hand, to: ANIM_LOC.table, cards: [SEVEN_S] },
        { type: ANIM_EVT.cover, seat: 1, from: ANIM_LOC.hand, to: ANIM_LOC.table, cards: [C(1, 6)] },
        { type: ANIM_EVT.refill, seat: 0, from: ANIM_LOC.deck, to: ANIM_LOC.hand, cards: [TRUMP] },
    ], 2, FINAL);
    assert.equal(p.steps[0].startMs, 0);
    assert.equal(p.steps[1].startMs, 0, 'one move is one movement; the kernel just spends an event per card');
    assert.deepEqual([p.steps[0].beatFirst, p.steps[0].beatN], [0, 2]);
    assert.deepEqual([p.steps[1].beatFirst, p.steps[1].beatN], [0, 2],
        'the host reads the span off either step, and merges its own list the same way');
    assert.deepEqual([p.steps[2].beatFirst, p.steps[2].beatN], [2, 1]);
    assert.equal(p.steps[2].startMs, TIME + GAP, 'the beat after a merged cover opens one stride in, not two');
});

test('an `out` is a notice: no flight, no time, and it costs the beat behind it nothing', () => {
    const p = animBuildPlan([
        { type: ANIM_EVT.pickup, seat: 1, from: ANIM_LOC.table, to: ANIM_LOC.hand, cards: [SIX_S] },
        { type: ANIM_EVT.out, seat: 0 },
        { type: ANIM_EVT.refill, seat: 0, from: ANIM_LOC.deck, to: ANIM_LOC.hand, cards: [TRUMP] },
    ], 2, FINAL);
    assert.equal(p.steps[1].durationMs, 0, 'an out moves no card, so it flies for no time');
    assert.equal(p.steps[1].startMs, TIME + GAP);
    assert.equal(p.steps[2].startMs, TIME + GAP, 'the refill opens where it would have with no out at all');
    assert.equal(p.totalMs, TIME + GAP + TIME, 'the sequence is two flights long, not three');
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

// ---- the reversal's order ---------------------------------------------------
// The rule AnimationContext's four queue-insertion branches gave way to: the
// board reverses what it must before it plays anything else, last group first,
// and a group nothing reverts is dropped rather than played as a beat of
// silence. The decision is transport-dependent and the order is not, which is
// why a server client reaches it with verdicts it has already asked for.

test('a doomed sequence flies home last group first', () => {
    const steps = animReversalOrder(
        [ANIM_CONFLICT_REVERT, ANIM_CONFLICT_REVERT, ANIM_CONFLICT_REVERT],
        [1, 1, 1]);
    assert.deepEqual(steps, [[2], [1], [0]], 'the cards travel back the way they came');
});

test('a group nothing reverts plays no beat of silence', () => {
    const steps = animReversalOrder(
        [ANIM_CONFLICT_REVERT, ANIM_CONFLICT_KEEP, ANIM_CONFLICT_CLEAR, ANIM_CONFLICT_REVERT],
        [2, 1, 1]);
    assert.deepEqual(steps, [[3], [0]],
        'the KEEP and the CLEAR build nothing, and the group left empty is dropped');
});

test('a reversal with nothing doomed is no reversal at all', () => {
    assert.deepEqual(animReversalOrder([ANIM_CONFLICT_KEEP, ANIM_CONFLICT_CLEAR], [1, 1]), [],
        'the common case - an arrival that vouches for everything - plays no theatre');
    assert.deepEqual(animReversalOrder([], []), []);
});

test('groups that do not account for the motions are refused, not half-reversed', () => {
    assert.throws(() => animReversalOrder([ANIM_CONFLICT_REVERT, ANIM_CONFLICT_REVERT], [1]),
        /anim_reversal_order error/);
});

test('a seatless notice keeps its seat out of the answer', () => {
    const b = animBuildBeats([{ type: ANIM_EVT.magic_transition }]);
    assert.equal(b.beats[0].seat, -1, 'ANIM_SEAT_NONE, not seat 255');
});
