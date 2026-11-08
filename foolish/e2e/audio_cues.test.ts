/* =============================================================================
 * SOUND CUES: the kernel's answer through the bridge, the web's delivery of it
 * =============================================================================
 * docs/AUDIO.md. Which beat makes which sound, and when, is the kernel's rule
 * and is pinned natively (c/tests/anim_plan_test.c test_plan_sound_cues, and
 * c/tests/tests.c test_plan_cues_over_real_games). This file holds the three
 * things that are the web's:
 *
 *   1. THE BRIDGE - the cues read out of the wasm's AnimPlan through the
 *      generated accessors, and the two scalar entries beside it. A rule that is
 *      right in C and wrong in the marshal is wrong in the speaker.
 *   2. THE HAND-OVER (src/audio/cueSchedule.ts) - every cue reaches the engine
 *      exactly once, ahead of time, pinned to the plan's own millisecond however
 *      raggedly the frames run, and a tab that was away does not replay what it
 *      missed.
 *   3. THE ENGINE (src/audio/cueAudio.ts) against a stand-in AudioContext - it
 *      schedules on the audio clock, and every way it is meant to be silent is
 *      silent rather than broken: no Web Audio, muted, hidden, a file missing.
 *   4. THE TABLE (src/audio/cueSounds.ts) - every cue the kernel can make has a
 *      row, and every placeholder a row names is one tools/soundgen writes.
 *
 * What the real page hands the engine during real moves is held in
 * e2e/ui_animation_trace.test.ts, where the page is.
 *
 * Pure kernel/wasm test - needs no Postgres.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { join, resolve } from 'node:path';

import {
    animBuildPlan, animCuesNext, animFinishCue, animRoleCue, ANIM_EVT, ANIM_LOC, type AnimPlanEventIn,
} from '../sdk/ts/wasm/bots.ts';
import { ANIM_TIME_MS, ANIM_GAP_MS, ANIM_BOUT_END_HOLD_MS } from '../sdk/ts/gen/anim.bots.ts';
import { CARD_NONE_SUIT, CARD_NONE_VALUE, GAME_STATUS_PLAYING, PLAYER_STATUS_IN, type TableView_Snap as TableView } from '../sdk/ts/gen/view_layout.bots.ts';
import {
    ANIM_CUE_NONE, ANIM_CUE_DEAL, ANIM_CUE_LIFT, ANIM_CUE_PLACE, ANIM_CUE_COVER, ANIM_CUE_SWEEP,
    ANIM_CUE_PILE, ANIM_CUE_GOOD, ANIM_CUE_HAND_OFF, ANIM_CUE_FINISH, ANIM_CUE_FOOL, ANIM_CUE_OUT,
    ANIM_CUE_NAMES, ANIM_N_CUES, ANIM_SOUND_STAGGER_MS, ANIM_SOUND_OUT_LAG_MS,
    ANIM_CUE_FLIP, ANIM_CUE_PICKUP, ANIM_CUE_REVERT, ANIM_CUE_DEAL_LAND, ANIM_CUE_PASS, ANIM_CUE_TURN,
} from '../sdk/ts/gen/kernel_consts.bots.ts';
import { EVW_MSG_ATTACKED, EVW_MSG_PASSED } from '../sdk/ts/gen/view_layout.bots.ts';
import { CUE_LATE_MS, CUE_LOOK_AHEAD_MS, cueKey, dueStrikes, type PlanCue } from '../src/audio/cueSchedule.ts';
import { CUE_SOUNDS, cueLeadMs, preludesFor, takesFor, wantedLevel } from '../src/audio/cueSounds.ts';
import { CueAudio } from '../src/audio/cueAudio.ts';
import { finishCueOf } from '../src/audio/useFinishCue.ts';
import { cuesNextFor } from '../src/audio/useCuePriority.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const REPO = resolve(import.meta.dirname, '..');
const C = (suit: number, value: number) => ({ suit, value });
const lay = (type: number, seat: number, n: number): AnimPlanEventIn => ({
    type, seat, from: ANIM_LOC.hand, to: ANIM_LOC.table,
    cards: Array.from({ length: n }, (_, i) => C((seat + i) % 4, 6 + i)),
});
const draw = (seat: number, n: number): AnimPlanEventIn => ({
    type: ANIM_EVT.refill, seat, from: ANIM_LOC.deck, to: ANIM_LOC.hand,
    cards: Array.from({ length: n }, (_, i) => C(i % 4, 2 + i)),
});
const discard = (n: number): AnimPlanEventIn => ({
    type: ANIM_EVT.discard, from: ANIM_LOC.table, to: ANIM_LOC.discard,
    cards: Array.from({ length: n }, (_, i) => C(i % 4, 6 + i)),
});
/** A last defence: two covers in one move, the seat out, the sweep, two refills. */
const boutEnd = (): AnimPlanEventIn[] => [
    lay(ANIM_EVT.cover, 1, 1), lay(ANIM_EVT.cover, 1, 1), { type: ANIM_EVT.out, seat: 1 },
    discard(4), draw(0, 3), draw(1, 1),
];
// The board the page is holding: the plan reads how many sit from it, and
// nothing else of it when no step carries a board of its own. The cues read no
// board at all - they are laid out off the steps' types, counts and clock.
const TABLE: TableView = {
    status: GAME_STATUS_PLAYING, powerSuit: 2, firstAttacker: 0, defender: 1, mySeat: -1, fool: -1,
    deckCount: 12, discardPileLength: 0, hasFlipped: false, hasGoodTimestamp: false, drawn: false, passing: true,
    flipped: { suit: CARD_NONE_SUIT, value: CARD_NONE_VALUE }, goodMask: 0, version: 0, battles: [],
    seats: [0, 1].map(() => ({ status: PLAYER_STATUS_IN, handCount: 6, awaitingAttack: false, isAi: false, id: '', name: '' })),
    myHand: [], elimination: [], rated: false, ratingKnown: false, ratingDelta: new Array(8).fill(0), ratingBefore: new Array(8).fill(0),
    ratingAfter: new Array(8).fill(0), gameId: 'g', title: '',
};
const planOf = (events: AnimPlanEventIn[]) => animBuildPlan(events, TABLE);
const named = (cues: readonly PlanCue[]) =>
    cues.map((c) => `${ANIM_CUE_NAMES[c.kind]}@${c.atMs} step${c.step} x${c.hits} (${c.nCards})`
        + (c.undoes ? ` undoes ${ANIM_CUE_NAMES[c.undoes]}` : ''));
const bits = (...kinds: number[]) => kinds.reduce((m, k) => m | (1 << k), 0);
const namesIn = (mask: number) => Object.entries(ANIM_CUE_NAMES).filter(([k]) => mask & (1 << Number(k))).map(([, n]) => n);

// ---- 1. the bridge ------------------------------------------------------------------

test('the plan\'s cues cross the bridge field for field', () => {
    const plan = planOf([lay(ANIM_EVT.attack_pass, 0, 3), lay(ANIM_EVT.cover, 1, 1)]);
    const stride = ANIM_TIME_MS + ANIM_GAP_MS;
    assert.deepEqual(named(plan.cues), [
        `LIFT@0 step0 x1 (3)`,
        `PLACE@${ANIM_TIME_MS} step0 x3 (3)`,
        `LIFT@${stride} step1 x1 (1)`,
        `COVER@${stride + ANIM_TIME_MS} step1 x1 (1)`,
    ]);
});

test('a bout end reads back whole: the spread, the out behind its cover, the sweep after the rest', () => {
    const plan = planOf(boutEnd());
    const sweepAt = ANIM_TIME_MS + ANIM_GAP_MS + ANIM_BOUT_END_HOLD_MS;
    const dealAt = sweepAt + ANIM_TIME_MS + ANIM_GAP_MS;
    assert.deepEqual(named(plan.cues), [
        `LIFT@0 step0 x1 (1)`,
        `COVER@${ANIM_TIME_MS} step0 x1 (1)`,
        `COVER@${ANIM_TIME_MS + ANIM_SOUND_STAGGER_MS} step1 x1 (1)`,
        `OUT@${ANIM_TIME_MS + ANIM_SOUND_OUT_LAG_MS} step2 x1 (0)`,
        `SWEEP@${sweepAt} step3 x1 (4)`,
        `PILE@${sweepAt + ANIM_TIME_MS} step3 x1 (4)`,
        `DEAL@${dealAt} step4 x3 (3)`,
        `DEAL_LAND@${dealAt + ANIM_TIME_MS} step4 x3 (3)`,
        `DEAL@${dealAt + ANIM_TIME_MS + ANIM_GAP_MS} step5 x1 (1)`,
        `DEAL_LAND@${dealAt + 2 * ANIM_TIME_MS + ANIM_GAP_MS} step5 x1 (1)`,
    ]);
});

test('a transfer crosses the bridge as one: it lands, then slides on', () => {
    const pass = planOf([{ ...lay(ANIM_EVT.attack_pass, 0, 1), msg: EVW_MSG_PASSED }]);
    assert.deepEqual(named(pass.cues), [`LIFT@0 step0 x1 (1)`, `PLACE@${ANIM_TIME_MS} step0 x1 (1)`, `PASS@${ANIM_TIME_MS} step0 x1 (1)`]);
    const attack = planOf([{ ...lay(ANIM_EVT.attack_pass, 0, 1), msg: EVW_MSG_ATTACKED }]);
    assert.ok(!attack.cues.some((c) => c.kind === ANIM_CUE_PASS), 'an attack does not slide on');
});

test('a revert crosses the bridge with the cue it undoes', () => {
    const home = (type: number): AnimPlanEventIn => ({ ...lay(type, 0, 1), type: ANIM_EVT.revert, from: ANIM_LOC.table, to: ANIM_LOC.hand });
    const plan = planOf([lay(ANIM_EVT.attack_pass, 0, 1), home(0), lay(ANIM_EVT.cover, 0, 1), home(0)]);
    assert.deepEqual(named(plan.cues).filter((c) => c.startsWith('REVERT')), [
        `REVERT@${ANIM_TIME_MS + ANIM_GAP_MS} step1 x1 (1) undoes COVER`.replace('COVER', 'PLACE'),
        `REVERT@${3 * (ANIM_TIME_MS + ANIM_GAP_MS)} step3 x1 (1) undoes COVER`,
    ], 'a refused attack undoes PLACE and a refused cover undoes COVER; every other cue undoes nothing');
    assert.ok(plan.cues.filter((c) => c.kind !== ANIM_CUE_REVERT).every((c) => c.undoes === ANIM_CUE_NONE));
});

test('which cues could sound next is the kernel\'s answer, field for field', () => {
    const ask = { status: GAME_STATUS_PLAYING, mySeat: 0, nPlayers: 2, nBattles: 0, nCovered: 0, hasFlipped: true, deck: 20 };
    assert.deepEqual(namesIn(animCuesNext(ask)), ['LIFT', 'PLACE', 'PASS'], 'an empty table mid-game: a card can be laid');
    // Each scalar is told apart from its neighbour by a board where only it differs.
    assert.ok(animCuesNext({ ...ask, nBattles: 2, nCovered: 1 }) & (1 << ANIM_CUE_COVER), 'one of two attacks is uncovered');
    assert.ok(!(animCuesNext({ ...ask, nBattles: 2, nCovered: 2 }) & (1 << ANIM_CUE_COVER)), 'none is');
    assert.deepEqual(namesIn(animCuesNext({ ...ask, status: 0 })), ['DEAL', 'FLIP', 'DEAL_LAND', 'TURN'], 'a lobby (GAME_STATUS_WAITING) can only deal');
    const last = { ...ask, hasFlipped: false, deck: 0 };
    assert.ok(animCuesNext(last) & (1 << ANIM_CUE_FOOL), 'the stock gone: a seated viewer could be the fool');
    assert.ok(!(animCuesNext({ ...last, mySeat: -1 }) & (1 << ANIM_CUE_FOOL)), 'a spectator could not');
    assert.ok(!(animCuesNext({ ...last, deck: 1 }) & (1 << ANIM_CUE_OUT)), 'one card of stock and an empty table: nobody goes out');
    assert.ok(animCuesNext({ ...ask, nBattles: 1, deck: 11 }) & (1 << ANIM_CUE_OUT), 'two seats can draw twelve');
    assert.ok(!(animCuesNext({ ...ask, nBattles: 1, deck: 11, nPlayers: 1 }) & (1 << ANIM_CUE_OUT)), 'one seat cannot');
});

test('a board on screen is asked about with its own numbers', () => {
    // TABLE: playing, twelve in the stock and no trump turned, nothing down.
    assert.deepEqual(namesIn(cuesNextFor(TABLE)), ['DEAL', 'FLIP', 'LIFT', 'PLACE', 'DEAL_LAND', 'TURN', 'PASS']);
    const six = { suit: 0, value: 6 }, none = { suit: CARD_NONE_SUIT, value: CARD_NONE_VALUE };
    const mid: TableView = { ...TABLE, hasFlipped: true, deckCount: 20, mySeat: 1,
        battles: [{ attack: six, defense: { suit: 0, value: 9 } }, { attack: { suit: 1, value: 6 }, defense: none }] };
    assert.deepEqual(namesIn(cuesNextFor(mid)), ['DEAL', 'LIFT', 'PLACE', 'COVER', 'PICKUP', 'SWEEP', 'PILE', 'GOOD', 'HAND_OFF', 'DEAL_LAND', 'PASS', 'INTO_HAND']);
    assert.ok(!(cuesNextFor({ ...mid, battles: [mid.battles[0]] }) & (1 << ANIM_CUE_COVER)), 'a covered pile is counted as covered');
    assert.ok(cuesNextFor({ ...mid, deckCount: 0, hasFlipped: false }) & (1 << ANIM_CUE_FOOL), 'my seat is passed as mine');
    assert.ok(!(cuesNextFor({ ...mid, deckCount: 0, hasFlipped: false, mySeat: -1 }) & (1 << ANIM_CUE_FOOL)));
    assert.ok(!(cuesNextFor({ ...mid, deckCount: 11 }) & (1 << ANIM_CUE_OUT)) === false, 'two seats, twelve of stock with the trump');
    assert.ok(!(cuesNextFor({ ...mid, deckCount: 12 }) & (1 << ANIM_CUE_OUT)));
    assert.deepEqual(namesIn(cuesNextFor({ ...mid, status: 2 })), [], 'a finished game');
});

test('the role cue and the finish cue are the kernel\'s answers', () => {
    const shown = { defender: 1, firstAttacker: 0, goodMask: 0b001 };
    assert.equal(animRoleCue(shown, { ...shown }), ANIM_CUE_NONE);
    assert.equal(animRoleCue(shown, { ...shown, goodMask: 0b101 }), ANIM_CUE_GOOD);
    assert.equal(animRoleCue(shown, { ...shown, goodMask: 0 }), ANIM_CUE_NONE, 'a good cleared is silent');
    assert.equal(animRoleCue(shown, { ...shown, defender: 2 }), ANIM_CUE_HAND_OFF);
    // Argument order across the bridge: the FIRST three scalars are `shown`.
    assert.equal(animRoleCue({ ...shown, goodMask: 0b101 }, shown), ANIM_CUE_NONE,
        'a good taken away is not a good said, so the two triples are not swapped on the way in');
    // Asked as the finish order is: the elimination, a fool, the seats, me.
    assert.equal(animFinishCue([0, 1], -1, 3, 0), ANIM_CUE_NONE);
    assert.equal(animFinishCue([0, 1], 2, 3, 2), ANIM_CUE_FOOL);
    assert.equal(animFinishCue([0, 1], 2, 3, 0), ANIM_CUE_FINISH);
    assert.equal(animFinishCue([0, 1], 2, 3, -1), ANIM_CUE_FINISH);
    // A repetition draw: seat 0 went out and 1 and 2 are both fools, though
    // `gameOver` names only the lowest. The list crosses, not just that seat.
    assert.equal(animFinishCue([0], 1, 3, 2), ANIM_CUE_FOOL, 'every fool of a draw hears the fool\'s sound');
    assert.equal(animFinishCue([0], 1, 3, 0), ANIM_CUE_FINISH, 'and the seat that went out first does not');
});

test('a board\'s finish cue reads the end and the viewer off it, each as itself', () => {
    const seats = (n: number) => Array.from({ length: n }, () => ({}) as TableView['seats'][number]);
    const board = (elimination: number[], fool: number, n: number, mySeat: number) => ({ elimination, fool, seats: seats(n), mySeat });
    assert.equal(finishCueOf(board([], -1, 2, 0)), ANIM_CUE_NONE, 'a running game has no fool and makes no sound');
    assert.equal(finishCueOf(board([0], 1, 2, 1)), ANIM_CUE_FOOL);
    assert.equal(finishCueOf(board([0], 1, 2, 0)), ANIM_CUE_FINISH);
    // The one viewer for whom the two seats are not interchangeable: a spectator
    // has no seat, and still hears the game end.
    assert.equal(finishCueOf(board([0, 1], 2, 3, -1)), ANIM_CUE_FINISH);
    // A REPETITION DRAW of four: seat 2 went out, and 0, 1 and 3 are all fools.
    // The board's `fool` is the lowest of them, so a viewer at seat 3 is a fool
    // only because the seat count and the elimination crossed too.
    assert.equal(finishCueOf(board([2], 0, 4, 3)), ANIM_CUE_FOOL, 'a fool of a draw that `fool` does not name hears it');
    assert.equal(finishCueOf(board([2], 0, 4, 2)), ANIM_CUE_FINISH, 'the seat out before the draw hears the finish');
});

// ---- 2. the hand-over ---------------------------------------------------------------

/** Walk a plan's clock by `frameMs()` at a time and collect every strike with
 *  the run-clock millisecond it was aimed at. */
function walk(cues: readonly PlanCue[], endMs: number, frameMs: () => number, startMs = 0) {
    const done = new Set<number>();
    const struck: { kind: number; at: number; early: number }[] = [];
    for (let now = startMs; now <= endMs; now += frameMs()) {
        for (const s of dueStrikes(cues, now, done)) struck.push({ kind: s.kind, at: now + s.delayMs, early: s.delayMs });
    }
    return struck;
}
/** Every strike the plan asks for, expanded: the truth a walk is held to. */
const wanted = (cues: readonly PlanCue[]) => cues
    .flatMap((c) => Array.from({ length: c.hits }, (_, h) => ({ kind: c.kind, at: c.atMs + h * ANIM_SOUND_STAGGER_MS })))
    .sort((a, b) => a.at - b.at || a.kind - b.kind);
const byTime = <T extends { kind: number; at: number }>(xs: T[]) =>
    xs.map(({ kind, at }) => ({ kind, at })).sort((a, b) => a.at - b.at || a.kind - b.kind);

test('at 60 frames a second every strike is handed over once, aimed at the plan\'s own millisecond', () => {
    const plan = planOf(boutEnd());
    const struck = walk(plan.cues, plan.totalMs + 1000, () => 16);
    assert.deepEqual(byTime(struck), wanted(plan.cues));
    assert.equal(struck.length, 14, 'a lift, two covers, an out, the sweep, the pile, four dealt cards and their four landings (a refill of three is three strikes)');
});

test('ragged frames move no sound: the same strikes at the same milliseconds', () => {
    const plan = planOf(boutEnd());
    // 16ms frames with a 70ms stall every seventh: a layout, a GC, a busy tab.
    let i = 0;
    const struck = walk(plan.cues, plan.totalMs + 1000, () => (++i % 7 === 0 ? 70 : 16));
    assert.deepEqual(byTime(struck), wanted(plan.cues),
        'a strike is aimed at the cue\'s time, not at the frame that happened to hand it over');
    // …and that is only possible because they were handed over EARLY: a strike
    // with no lead is one a late frame has already made late.
    const late = struck.filter((s) => s.early === 0 && s.at > 0);
    assert.deepEqual(late, [], 'no strike but the run\'s very first was handed over with nothing in hand');
    assert.ok(struck.every((s) => s.early <= CUE_LOOK_AHEAD_MS + 5 * ANIM_SOUND_STAGGER_MS),
        'and none was handed over further ahead than the look-ahead plus its own spread');
});

test('a cue further off than the look-ahead waits for a later frame', () => {
    const cue: PlanCue = { atMs: 1000, kind: ANIM_CUE_SWEEP, step: 0, hits: 1, nCards: 7, undoes: 0 };
    const done = new Set<number>();
    assert.deepEqual(dueStrikes([cue], 1000 - CUE_LOOK_AHEAD_MS - 1, done), []);
    assert.equal(done.size, 0, 'and it is not marked as handed over');
    assert.deepEqual(dueStrikes([cue], 1000 - CUE_LOOK_AHEAD_MS, done),
        [{ kind: ANIM_CUE_SWEEP, delayMs: CUE_LOOK_AHEAD_MS, nCards: 7, undoes: 0, step: 0 }],
        'with how far off it is and how many cards it stands for');
    assert.deepEqual(dueStrikes([cue], 990, done), [], 'once handed over it is never handed over again');
});

test('a cue whose file starts before its moment is handed over that much earlier', () => {
    const cue: PlanCue = { atMs: 2000, kind: ANIM_CUE_SWEEP, step: 2, hits: 1, nCards: 7, undoes: 0 };
    const lead = (kind: number) => (kind === ANIM_CUE_SWEEP ? 1200 : 0);
    const done = new Set<number>();
    assert.deepEqual(dueStrikes([cue], 2000 - 1200 - CUE_LOOK_AHEAD_MS - 1, done, lead), [], 'not before its file has to start');
    assert.deepEqual(dueStrikes([cue], 2000 - 1200 - CUE_LOOK_AHEAD_MS, done, lead),
        [{ kind: ANIM_CUE_SWEEP, delayMs: 1200 + CUE_LOOK_AHEAD_MS, nCards: 7, undoes: 0, step: 2 }],
        'and then with the whole lead in hand, aimed at the moment itself');
    // The real table's leads: the deal's thumb slide and grab, the throw-in's.
    assert.ok(cueLeadMs(ANIM_CUE_DEAL) > 150 && cueLeadMs(ANIM_CUE_LIFT) > 150, 'the deal and a lift start well before the card leaves');
    assert.ok(cueLeadMs(ANIM_CUE_SWEEP) > 2000 && cueLeadMs(ANIM_CUE_PICKUP) > 2000, 'and the strokes before a sweep or a pickup, seconds');
    assert.ok(cueLeadMs(ANIM_CUE_PASS) <= 0, 'the slide on starts after its landing: no lead');
});

test('a tab that was away does not replay what it missed', () => {
    const plan = planOf(boutEnd());
    // The loop ran for 200ms, then nothing for five seconds (a hidden tab gets
    // no frames), then it comes back to a run that is long over.
    const done = new Set<number>();
    const before = [0, 16, 32, 200].flatMap((now) => [...dueStrikes(plan.cues, now, done)]);
    assert.deepEqual(before.map((s) => s.kind), [ANIM_CUE_LIFT], 'only the lift was due in the first 200ms');
    const back = dueStrikes(plan.cues, 5200, done);
    assert.deepEqual(back, [], 'nothing it missed is played in a burst');
    assert.equal(done.size, plan.cues.length, 'and every cue is settled, so none of them comes back later either');
});

test('a strike is dropped once it is late, and kept while it is merely just behind', () => {
    const cue: PlanCue = { atMs: 500, kind: ANIM_CUE_PLACE, step: 3, hits: 3, nCards: 3, undoes: 0 };
    // 500, 545, 590. At 500 + LATE + 1 the first is too old and the other two are not.
    const got = dueStrikes([cue], 500 + CUE_LATE_MS + 1, new Set());
    assert.equal(got.length, 2, 'the strike more than CUE_LATE_MS behind is dropped, its later siblings are not');
    assert.ok(got.every((s) => s.delayMs === 0 || s.delayMs > 0));
    assert.equal(dueStrikes([cue], 500 + CUE_LATE_MS, new Set()).length, 3, 'exactly CUE_LATE_MS behind still plays');
});

test('a revert is handed over with the cue it undoes', () => {
    const cue: PlanCue = { atMs: 0, kind: ANIM_CUE_REVERT, step: 0, hits: 1, nCards: 1, undoes: ANIM_CUE_COVER };
    assert.deepEqual(dueStrikes([cue], 0, new Set()), [{ kind: ANIM_CUE_REVERT, delayMs: 0, nCards: 1, undoes: ANIM_CUE_COVER, step: 0 }]);
});

test('a cue is remembered by its step and kind, the pair the kernel keeps unique', () => {
    const plan = planOf(boutEnd());
    const keys = plan.cues.map(cueKey);
    assert.equal(new Set(keys).size, keys.length);
    // A run appended to in flight: the cues already handed over are not handed
    // over again by the longer plan, and the new ones are.
    const short = planOf(boutEnd().slice(0, 1));
    const done = new Set<number>();
    dueStrikes(short.cues, 600, done);
    const more = dueStrikes(planOf(boutEnd()).cues, 600, done);
    assert.deepEqual(more.map((s) => s.kind).sort(), [ANIM_CUE_COVER, ANIM_CUE_OUT].sort(),
        'the second cover and the out are new; the lift and the first cover are not struck twice');
});

// ---- 3. the engine --------------------------------------------------------------------

interface FakeSource { buffer: unknown; playbackRate: { value: number }; startedAt: number | null; offset: number; stopped: boolean; gain: number; onended: (() => void) | null }
/** The anchor of the take a source plays (its buffer names its file). */
const anchorOf = (s: FakeSource): number => {
    const file = (s.buffer as { url: string }).url;
    for (const sound of Object.values(CUE_SOUNDS)) for (const t of sound?.takes ?? []) if (t.file === file) return t.anchorMs;
    throw new Error(`no take plays ${file}`);
};
/** Every file a cue's row names. */
const filesOf = (...kinds: number[]) => kinds.flatMap((k) => (CUE_SOUNDS[k]?.takes ?? []).map((t) => t.file));

/** A stand-in AudioContext: a clock the test owns and a record of what was asked of it. */
function fakeAudio(opts: { state?: string; fail?: (url: string) => boolean; hold?: boolean } = {}) {
    const sources: FakeSource[] = [];
    const fetched: string[] = [];
    /** Held fetches, oldest first: each answers when the test releases it. */
    const held: { url: string; go: () => void }[] = [];
    let open = 0, widest = 0;
    // A buffer is four samples, a second long, and says which file it came from.
    const samples = (n: number, from: number, url = '') => { const d = Float32Array.from({ length: n }, (_, i) => from + i);
        return { numberOfChannels: 1, length: n, sampleRate: n, duration: 1, data: d, url, getChannelData: () => d }; };
    let decodedN = 0;
    let created = 0;
    const ctxs: any[] = [];
    class Ctx {
        currentTime = 10;
        state = opts.state ?? 'running';
        destination = {};
        masterGain: { value: number; setValueAtTime(v: number): void } | null = null;
        constructor() { created++; ctxs.push(this); }
        resume() { this.state = 'running'; return Promise.resolve(); }
        createGain() {
            const node: any = { gain: { value: 1, setValueAtTime(v: number) { node.gain.value = v; } }, connected: null as unknown,
                connect(to: unknown) { node.connected = to; }, disconnect() {} };
            if (!this.masterGain) this.masterGain = node.gain;
            return node;
        }
        createBufferSource() {
            const s: any = { buffer: null, playbackRate: { value: 1 }, startedAt: null, stopped: false, gain: 0, onended: null,
                connect(g: any) { s.gainNode = g; },
                start(when: number, offset = 0) { s.startedAt = when; s.offset = offset; s.gain = s.gainNode.gain.value; },
                stop() { s.stopped = true; } };
            sources.push(s);
            return s;
        }
        // Every file decodes to four samples of its own, so a buffer can be told from its reverse.
        decodeAudioData(bytes: ArrayBuffer, ok: (b: unknown) => void) { ok(samples(4, 10 * ++decodedN, new TextDecoder().decode(bytes))); }
        createBuffer(_channels: number, length: number) { return samples(length, 0); }
    }
    const g = globalThis as any;
    const saved = { AudioContext: g.AudioContext, fetch: g.fetch, localStorage: g.localStorage, document: g.document };
    const storage = new Map<string, string>();
    g.AudioContext = Ctx;
    g.fetch = (url: string) => {
        fetched.push(url);
        widest = Math.max(widest, ++open);
        const answer = () => { open--; return opts.fail?.(url)
            ? { ok: false, status: 404, arrayBuffer: () => Promise.resolve(new ArrayBuffer(0)) }
            : { ok: true, status: 200, arrayBuffer: () => Promise.resolve(new TextEncoder().encode(url.replace('/audio/', '')).buffer) }; };
        if (!opts.hold) return Promise.resolve().then(answer);
        return new Promise((ok) => { held.push({ url, go: () => ok(answer()) }); });
    };
    g.localStorage = { getItem: (k: string) => storage.get(k) ?? null, setItem: (k: string, v: string) => { storage.set(k, v); } };
    g.document = { hidden: false };
    return {
        sources, fetched, storage, ctxs, created: () => created, doc: g.document as { hidden: boolean },
        widest: () => widest, waiting: () => held.map((h) => h.url.replace('/audio/gen/', '')),
        /** Answer the `n` oldest held fetches and let what follows run. */
        release: async (n = 1) => { for (const h of held.splice(0, n)) h.go(); for (let i = 0; i < 8; i++) await new Promise((r) => setImmediate(r)); },
        /** Release held fetches one at a time until `done` (default: none is left). Bounded: a
         *  queue that never empties is a failure to report, not a test to hang in. */
        drain: async (done: () => boolean = () => held.length === 0) => {
            for (let i = 0; i < 400 && !done(); i++) {
                assert.ok(held.length > 0, 'the engine stopped asking for files before the test was satisfied');
                held.shift()!.go();
                for (let k = 0; k < 8; k++) await new Promise((r) => setImmediate(r));
            }
            assert.ok(done(), 'four hundred fetches were answered and the test is still waiting');
        },
        /** Let every fetch and decode finish. */
        settle: async () => { for (let i = 0; i < 30; i++) await new Promise((r) => setImmediate(r)); },
        restore: () => { Object.assign(g, saved); if (saved.AudioContext === undefined) delete g.AudioContext; },
    };
}

test('with no Web Audio at all the engine is a no-op, not a crash', () => {
    const g = globalThis as any;
    assert.equal(g.AudioContext, undefined, 'Node has no AudioContext, which is the case under test');
    const engine = new CueAudio();
    // Node has a fetch of its own; with no Web Audio it must never be reached.
    const realFetch = g.fetch;
    let fetches = 0;
    g.fetch = () => { fetches++; return Promise.reject(new Error('no')); };
    engine.arm();
    engine.want(bits(ANIM_CUE_PLACE));
    engine.unlock();
    assert.equal(engine.strike(ANIM_CUE_PLACE), false);
    g.fetch = realFetch;
    assert.equal(fetches, 0, 'nothing is downloaded for an engine that could never play it');
    engine.cancel();
    engine.setVolume(0.3);
    engine.setMuted(true);
    engine.setMuted(false);
    assert.deepEqual(engine.getState(), { muted: false, volume: 0.3 });
});

test('a strike is a source started on the audio clock, that many milliseconds ahead', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        engine.random = () => 0.5;      // dead centre: no pitch or level wander
        assert.equal(engine.strike(ANIM_CUE_PLACE), false, 'before a gesture there is no context and nothing plays');
        assert.equal(fake.created(), 0);
        engine.unlock();
        assert.equal(fake.created(), 1);
        await fake.settle();
        assert.equal(engine.strike(ANIM_CUE_PLACE, 120), true);
        const s = fake.sources[fake.sources.length - 1];
        assert.ok(anchorOf(s) > 0 && anchorOf(s) < 120, 'a landing\'s take has its peak a little way in');
        assert.ok(Math.abs(s.startedAt! - (10 + 0.12 - anchorOf(s) / 1000)) < 1e-9,
            'currentTime + 120ms, less the take\'s anchor: scheduled so that its moment falls on the cue\'s');
        assert.equal(s.offset, 0, 'and played from its first sample');
        assert.equal(s.playbackRate.value, 1);
        assert.equal(s.gain, CUE_SOUNDS[ANIM_CUE_PLACE]!.gain, 'at the level the table gives this cue');
        engine.unlock();
        assert.equal(fake.created(), 1, 'a second gesture does not make a second context');
    } finally { fake.restore(); }
});

test('every file the table names is fetched once, from /audio/', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        engine.unlock();
        await fake.settle();
        const files = Object.values(CUE_SOUNDS).flatMap((s) => (s?.takes ?? []).map((t) => t.file));
        assert.deepEqual([...fake.fetched].sort(), files.map((f) => `/audio/${f}`).sort());
        for (const f of files) assert.ok(engine.loaded(f), `${f} decoded`);
    } finally { fake.restore(); }
});

test('pitch and level wander only as far as the table lets them, and a note does not wander at all', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        engine.unlock();
        await fake.settle();
        const place = CUE_SOUNDS[ANIM_CUE_HAND_OFF]!;
        assert.ok(place.pitch > 0, 'a stand-in wanders, which is the case under test');
        for (const r of [0, 0.999999]) {
            engine.random = () => r;
            assert.ok(engine.strike(ANIM_CUE_HAND_OFF));
            const s = fake.sources[fake.sources.length - 1];
            assert.ok(Math.abs(s.playbackRate.value - 1) <= place.pitch + 1e-9 && Math.abs(s.playbackRate.value - 1) > place.pitch * 0.9,
                `at the edge of the dice the pitch is at the edge of its range (${s.playbackRate.value})`);
            assert.ok(Math.abs(s.gain / place.gain - 1) <= place.level + 1e-9 && Math.abs(s.gain / place.gain - 1) > place.level * 0.9,
                `and so is the level (${s.gain})`);
        }
        engine.random = () => 0;
        assert.ok(engine.strike(ANIM_CUE_GOOD));
        const note = fake.sources[fake.sources.length - 1];
        assert.equal(note.playbackRate.value, 1, 'a cue with a pitch to keep keeps it');
        assert.equal(note.gain, CUE_SOUNDS[ANIM_CUE_GOOD]!.gain);
        engine.random = () => 0;
        assert.ok(engine.strike(ANIM_CUE_PLACE));
        const model = fake.sources[fake.sources.length - 1];
        assert.equal(model.playbackRate.value, 1, 'nor does a modelled card sound: its takes already vary as the recordings did');
        assert.equal(model.gain, CUE_SOUNDS[ANIM_CUE_PLACE]!.gain, 'in pitch and in level');
    } finally { fake.restore(); }
});

test('a sweep and a pickup are gathered in a stroke per two and a half cards, and a lift with no time plays its tight take', () => {
    for (const kind of [ANIM_CUE_SWEEP, ANIM_CUE_PICKUP]) {
        const sound = CUE_SOUNDS[kind]!;
        assert.equal(sound.cardsPerLevel, 2.5, 'a stroke gathers about two and a half cards (disc_game.py)');
        assert.deepEqual([1, 4, 10, 30].map((n) => wantedLevel(sound, n)), [1, 2, 4, 5], 'one to five strokes');
        assert.ok(preludesFor(sound, 10).length > 0 && preludesFor(sound, 10).every((t) => t.role === 'prelude' && t.level === 4),
            'ten cards are four strokes, played before the take');
        assert.ok(takesFor(sound, 0).every((t) => t.role === 'main'), 'the take itself is the lift');
        assert.deepEqual(preludesFor(sound, 0), [], 'no cards, no strokes');
    }
    const lift = CUE_SOUNDS[ANIM_CUE_LIFT]!;
    const longest = Math.max(...lift.takes.filter((t) => t.role === 'main').map((t) => t.anchorMs));
    assert.ok(takesFor(lift, longest).every((t) => t.role === 'main'), 'time for the whole gesture: an ordinary take');
    const some = takesFor(lift, 300);
    assert.ok(some.every((t) => t.role === 'main' && t.anchorMs <= 300), 'less: only the ordinary takes that fit');
    assert.ok(takesFor(lift, 0).every((t) => t.role === 'tight'), 'none: a tight one, the grab and slide pressed to the moment');
    assert.ok(Math.max(...takesFor(lift, 0).map((t) => t.anchorMs)) < longest, 'which comes in sooner');
    assert.ok(takesFor(lift, 0, true).every((t) => t.role === 'main'), 'and a revert plays the ordinary gesture, backwards');
    const place = CUE_SOUNDS[ANIM_CUE_PLACE]!;
    assert.ok(takesFor(place, 0).length === place.takes.length, 'a cue with no tight takes plays any of its own');
});

test('a take is played whole: its anchor early when there is time, from now when there is not', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        engine.random = () => 0.5;
        engine.unlock();
        await fake.settle();
        const roleOf = (s: FakeSource) => {
            const file = (s.buffer as { url: string }).url;
            for (const sound of Object.values(CUE_SOUNDS)) for (const t of sound?.takes ?? []) if (t.file === file) return t.role;
            return null;
        };
        // A lift's grab and thumb slide come before the card leaves.
        assert.ok(engine.strike(ANIM_CUE_LIFT, 1000));
        let s = fake.sources[fake.sources.length - 1];
        assert.ok(anchorOf(s) > 150 && roleOf(s) === 'main');
        assert.ok(Math.abs(s.startedAt! - (11 - anchorOf(s) / 1000)) < 1e-9, 'early enough to be heard whole on time');
        // Struck for NOW, as a run's first move is: a tight take, whole, from now.
        assert.ok(engine.strike(ANIM_CUE_LIFT, 0));
        s = fake.sources[fake.sources.length - 1];
        assert.equal(roleOf(s), 'tight', 'the grab and slide pressed closer to the moment');
        assert.equal(s.startedAt, 10, 'started at once');
        assert.ok(fake.sources.every((x) => x.offset === 0), 'and nothing is ever started part way into its file');
        // A negative anchor: the slide on starts after its landing.
        assert.ok(engine.strike(ANIM_CUE_PASS, 100));
        s = fake.sources[fake.sources.length - 1];
        assert.ok(anchorOf(s) < 0 && s.startedAt! > 10.1, 'a take that comes after its moment starts after it');
        // A landing at a far seat is quieter by the gain it is struck with.
        assert.ok(engine.strike(ANIM_CUE_DEAL_LAND, 100, 1, 0, 0.5));
        s = fake.sources[fake.sources.length - 1];
        assert.equal(s.gain, CUE_SOUNDS[ANIM_CUE_DEAL_LAND]!.gain * 0.5, 'a far seat\'s landing is struck at that seat\'s gain');
        // A sweep of ten cards: four strokes, then the lift on its moment.
        let before = fake.sources.length;
        assert.ok(engine.strike(ANIM_CUE_SWEEP, 5000, 10));
        let [lift, strokes] = fake.sources.slice(before);
        assert.equal(fake.sources.length - before, 2, 'two files: the lift and the strokes before it');
        assert.equal(roleOf(lift), 'main');
        assert.equal(roleOf(strokes), 'prelude');
        assert.ok(Math.abs(lift.startedAt! - (15 - anchorOf(lift) / 1000)) < 1e-9, 'the lift on the sweep\'s moment');
        assert.ok(Math.abs(strokes.startedAt! - (15 - anchorOf(strokes) / 1000)) < 1e-9 && anchorOf(strokes) > 1000,
            'and the strokes seconds before it, whole');
        // No time at all: the strokes start now, whole, and the lift keeps its moment.
        before = fake.sources.length;
        assert.ok(engine.strike(ANIM_CUE_PICKUP, 300, 10));
        [lift, strokes] = fake.sources.slice(before);
        assert.equal(strokes.startedAt, 10, 'the strokes from now, over whatever else is sounding');
        assert.ok(Math.abs(lift.startedAt! - (10.3 - anchorOf(lift) / 1000)) < 1e-9, 'and the lift still on its own moment');
    } finally { fake.restore(); }
});

test('two files are two takes: the same one is never played twice running', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        engine.unlock();
        await fake.settle();
        assert.ok(CUE_SOUNDS[ANIM_CUE_DEAL]!.takes.length >= 2, 'the deal has several takes, which is the case under test');
        let last: unknown = null;
        for (let i = 0; i < 12; i++) {
            assert.ok(engine.strike(ANIM_CUE_DEAL));
            const b = fake.sources[fake.sources.length - 1].buffer;
            assert.notEqual(b, last, `play ${i} repeated the take before it`);
            last = b;
        }
    } finally { fake.restore(); }
});

test('muted is silent, is remembered, and fetches nothing', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        engine.setMuted(true);
        engine.want(bits(ANIM_CUE_DEAL));
        engine.unlock();
        await fake.settle();
        assert.equal(fake.created(), 0, 'a muted player never opens an audio context');
        assert.deepEqual(fake.fetched, [], 'or downloads a sound');
        assert.equal(engine.strike(ANIM_CUE_PLACE), false);
        assert.equal(fake.storage.get('foolish_sound_muted'), '1');
        // A new page load in the same browser.
        assert.equal(new CueAudio().getState().muted, true, 'the next visit starts muted');
        // Unmuting is the gesture that starts it.
        engine.setMuted(false);
        await fake.settle();
        assert.equal(fake.created(), 1);
        assert.equal(engine.strike(ANIM_CUE_PLACE), true);
        assert.equal(fake.storage.get('foolish_sound_muted'), '0');
    } finally { fake.restore(); }
});

test('muting stops what was already scheduled', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        engine.unlock();
        await fake.settle();
        engine.strike(ANIM_CUE_SWEEP, 3000);
        const s = fake.sources[fake.sources.length - 1];
        assert.equal(s.stopped, false);
        engine.setMuted(true);
        assert.equal(s.stopped, true, 'a sound scheduled 3 s out does not play after the player hit mute');
        const before = fake.sources.length;
        assert.equal(engine.strike(ANIM_CUE_SWEEP), false, 'and with the context already open, a muted strike is still refused');
        assert.equal(fake.sources.length, before, 'nothing is even built for it');
    } finally { fake.restore(); }
});

test('the master volume is the level of everything, and is remembered', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        engine.unlock();
        await fake.settle();
        assert.equal(fake.ctxs[0].masterGain.value, 0.7, 'the default');
        engine.setVolume(0.25);
        assert.equal(fake.ctxs[0].masterGain.value, 0.25);
        assert.equal(fake.storage.get('foolish_sound_volume'), '0.25');
        assert.equal(new CueAudio().getState().volume, 0.25);
        engine.setVolume(7);
        assert.equal(engine.getState().volume, 1, 'clamped');
    } finally { fake.restore(); }
});

test('a hidden tab makes no sound', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        engine.unlock();
        await fake.settle();
        fake.doc.hidden = true;
        const before = fake.sources.length;
        assert.equal(engine.strike(ANIM_CUE_PLACE), false);
        assert.equal(fake.sources.length, before, 'nothing was even scheduled');
        fake.doc.hidden = false;
        assert.equal(engine.strike(ANIM_CUE_PLACE), true);
    } finally { fake.restore(); }
});

test('a context the browser has suspended makes no sound until a gesture wakes it', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        engine.unlock();
        await fake.settle();
        fake.ctxs[0].state = 'suspended';     // a phone call, a trip to another app
        assert.equal(engine.strike(ANIM_CUE_PLACE), false);
        engine.unlock();
        assert.equal(fake.ctxs[0].state, 'running');
        assert.equal(engine.strike(ANIM_CUE_PLACE), true);
    } finally { fake.restore(); }
});

test('a file that does not arrive silences its cue and no other', async () => {
    const fake = fakeAudio({ fail: (url) => url.includes('sweep') });
    try {
        const engine = new CueAudio();
        engine.unlock();
        await fake.settle();
        assert.equal(engine.strike(ANIM_CUE_SWEEP), false);
        assert.equal(engine.strike(ANIM_CUE_PILE), true);
    } finally { fake.restore(); }
});

// ---- 3b. the order the files arrive in ---------------------------------------------------

const short = (urls: string[]) => urls.map((u) => u.replace('/audio/gen/', ''));
const ALL_FILES = Object.values(CUE_SOUNDS).flatMap((x) => (x?.takes ?? []).map((t) => t.file)).map((f) => f.replace('gen/', ''));
const bare = (files: string[]) => files.map((f) => f.replace('gen/', ''));

test('before a gesture only the wanted cues are fetched, as bytes, and nothing at all unasked', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        await fake.settle();
        assert.deepEqual(fake.fetched, [], 'an engine nobody has told anything fetches nothing');
        engine.want(bits(ANIM_CUE_DEAL, ANIM_CUE_FLIP));
        await fake.settle();
        const lobby = bare(filesOf(ANIM_CUE_DEAL, ANIM_CUE_FLIP));
        assert.deepEqual(short(fake.fetched), lobby, 'a lobby\'s files, and only those');
        assert.equal(fake.created(), 0, 'with no audio context: a download is not a sound');
        assert.equal(engine.loaded(filesOf(ANIM_CUE_DEAL)[0]), false, 'so they are bytes, not yet buffers');
        // The gesture: what was fetched is decoded, not fetched again, and the rest follows.
        engine.unlock();
        assert.equal(engine.strike(ANIM_CUE_DEAL), true, 'the very tap that unlocks audio can strike a wanted cue');
        await fake.settle();
        assert.equal(fake.sources.length, 1, 'and it is heard, off the bytes that were already here');
        assert.deepEqual([...short(fake.fetched)].sort(), [...ALL_FILES].sort(), 'every file once, the lobby\'s included');
        assert.deepEqual(short(fake.fetched).slice(0, lobby.length), lobby);
    } finally { fake.restore(); }
});

test('the wanted cues are fetched first, a few at a time, and the rest in the table\'s order', async () => {
    const fake = fakeAudio({ hold: true });
    try {
        const engine = new CueAudio();
        engine.want(bits(ANIM_CUE_COVER, ANIM_CUE_PICKUP));
        engine.unlock();
        const wantedFiles = bare(filesOf(ANIM_CUE_PICKUP, ANIM_CUE_COVER).sort((a, b) => ALL_FILES.indexOf(a.replace('gen/', '')) - ALL_FILES.indexOf(b.replace('gen/', ''))));
        assert.deepEqual(fake.waiting(), wantedFiles.slice(0, 3), 'three at once, and they are wanted ones');
        await fake.drain();
        assert.equal(fake.widest(), 3, 'never more than three in flight');
        assert.deepEqual(short(fake.fetched), [...wantedFiles, ...ALL_FILES.filter((f) => !wantedFiles.includes(f))],
            'every wanted file first, in the table\'s order, and then the rest in it');
    } finally { fake.restore(); }
});

test('a board that changes re-orders what has not started, and disturbs nothing that has', async () => {
    const fake = fakeAudio({ hold: true });
    try {
        const engine = new CueAudio();
        engine.unlock();
        const first = ALL_FILES.slice(0, 3);
        assert.deepEqual(fake.waiting(), first, 'nothing wanted: the table\'s order');
        // The board moves on: now the end of a game is what could be heard next.
        engine.want(bits(ANIM_CUE_FINISH, ANIM_CUE_FOOL));
        assert.deepEqual(fake.waiting(), first, 'the three already on their way are left alone');
        await fake.release(1);
        assert.equal(short(fake.fetched)[3], 'finish_0.wav', 'the next place goes to a wanted file, far ahead of it in the table');
        await fake.release(1);
        assert.equal(short(fake.fetched)[4], 'fool_0.wav');
        engine.want(bits(ANIM_CUE_FINISH, ANIM_CUE_FOOL, ANIM_CUE_DEAL));
        engine.want(0);
        await fake.drain();
        assert.equal(new Set(fake.fetched).size, fake.fetched.length, 'no file was asked for twice, wanted again or not');
        assert.equal(fake.fetched.length, ALL_FILES.length, 'and every file was asked for');
    } finally { fake.restore(); }
});

test('a strike whose file is on its way waits for it, and is dropped if the wait was too long', async () => {
    const fake = fakeAudio({ hold: true });
    try {
        const engine = new CueAudio();
        engine.random = () => 0.5;
        engine.unlock();
        // In time: struck 50ms out, the file lands while that moment is still ahead.
        assert.equal(engine.strike(ANIM_CUE_GOOD, 50), true, 'waiting is not silence');
        assert.equal(fake.sources.length, 0);
        await fake.drain(() => filesOf(ANIM_CUE_GOOD).every((f) => engine.loaded(f)));
        assert.equal(fake.sources.length, 1, 'it starts as its file decodes');
        assert.equal(fake.sources[0].startedAt, 10 + 0.05, 'at the moment it was aimed at, not at the moment the file arrived');
        // A little late: the clock is 60ms past the aim when the file lands. Played, now.
        assert.equal(engine.strike(ANIM_CUE_FOOL, 0), true);
        fake.ctxs[0].currentTime = 10.06;
        await fake.drain(() => filesOf(ANIM_CUE_FOOL).every((f) => engine.loaded(f)));
        assert.equal(fake.sources.length, 2, '60ms behind is still a sound');
        assert.equal(fake.sources[1].startedAt, 10.06, 'started at once, since its moment has passed');
        assert.equal(fake.sources[1].offset, 0, 'and whole');
        // Too late: half a second behind. Dropped, and never played afterwards.
        assert.equal(engine.strike(ANIM_CUE_FINISH, 0), true);
        fake.ctxs[0].currentTime = 10.6;
        await fake.drain(() => filesOf(ANIM_CUE_FINISH).every((f) => engine.loaded(f)));
        assert.equal(fake.sources.length, 2, 'a sound half a second late is a mistake, so it is not made');
        await fake.settle();
        assert.equal(fake.sources.length, 2);
    } finally { fake.restore(); }
});

test('a cue that is struck jumps the queue, whatever the board said', async () => {
    const fake = fakeAudio({ hold: true });
    try {
        const engine = new CueAudio();
        engine.unlock();
        assert.equal(engine.strike(ANIM_CUE_FOOL), true);
        await fake.release(1);
        assert.equal(short(fake.fetched)[3], 'fool_0.wav', 'a file at the end of the table is the next one fetched');
    } finally { fake.restore(); }
});

test('muting stops the queue where it is, and unmuting picks it up again', async () => {
    const fake = fakeAudio({ hold: true });
    try {
        const engine = new CueAudio();
        engine.unlock();
        assert.equal(fake.waiting().length, 3);
        engine.setMuted(true);
        await fake.release(3);
        assert.deepEqual(fake.waiting(), [], 'the three on their way finish, and a muted player is sent no more');
        assert.equal(fake.fetched.length, 3);
        engine.setMuted(false);
        assert.equal(fake.waiting().length, 3, 'unmuting carries on from the fourth file');
        await fake.drain();
        assert.equal(new Set(fake.fetched).size, ALL_FILES.length, 'and every file arrives, once');
        assert.equal(fake.fetched.length, ALL_FILES.length);
        assert.equal(fake.created(), 1);
    } finally { fake.restore(); }
});

test('a strike that was waiting does not outlive a cancel', async () => {
    const fake = fakeAudio({ hold: true });
    try {
        const engine = new CueAudio();
        engine.unlock();
        engine.strike(ANIM_CUE_FOOL, 20);
        engine.cancel();
        await fake.drain(() => filesOf(ANIM_CUE_FOOL).every((f) => engine.loaded(f)));
        assert.equal(fake.sources.length, 0, 'a seek or a mute takes the waiting strikes with it');
    } finally { fake.restore(); }
});

// ---- 3c. a revert is the undone cue, backwards ---------------------------------------------

test('a revert plays the undone cue\'s own file reversed, at that cue\'s level, and reverses it once', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        engine.random = () => 0.5;
        engine.unlock();
        await fake.settle();
        // One take each way, so the reversed buffer can be held to its own forwards.
        const one = { ...CUE_SOUNDS[ANIM_CUE_PICKUP]! };
        const pickup = one.takes[0];
        (CUE_SOUNDS as Record<number, unknown>)[ANIM_CUE_PICKUP] = { ...one, takes: [pickup] };
        try {
            assert.ok(engine.strike(ANIM_CUE_PICKUP, 500));
            const forwards = fake.sources[0].buffer as any;
            assert.ok(engine.strike(ANIM_CUE_REVERT, 1500, 1, ANIM_CUE_PICKUP));
            const back = fake.sources[1];
            assert.notEqual(back.buffer, forwards, 'not the same buffer');
            assert.deepEqual([...(back.buffer as any).data], [...forwards.data].reverse(), 'the same samples, last first');
            assert.equal(back.gain, one.gain, 'at the level of the cue it undoes');
            assert.ok(Math.abs(back.startedAt! - (11.5 - (1 - pickup.anchorMs / 1000))) < 1e-9,
                'its anchor measured from the file\'s other end: the reversed take peaks on the revert\'s moment');
            assert.deepEqual([...forwards.data], [...(fake.sources[0].buffer as any).data], 'and the forward buffer is not disturbed');
            assert.ok(engine.strike(ANIM_CUE_REVERT, 0, 1, ANIM_CUE_PICKUP));
            assert.equal(fake.sources[2].buffer, back.buffer, 'the reversed buffer is made once and kept');
            assert.ok(engine.strike(ANIM_CUE_PICKUP));
            assert.equal(fake.sources[3].buffer, forwards, 'and the cue itself still plays forwards');
        } finally { (CUE_SOUNDS as Record<number, unknown>)[ANIM_CUE_PICKUP] = one; }
    } finally { fake.restore(); }
});

test('a revert that had to wait for its file is still played backwards', async () => {
    const fake = fakeAudio({ hold: true });
    try {
        const engine = new CueAudio();
        engine.unlock();
        assert.equal(engine.strike(ANIM_CUE_REVERT, 30, 1, ANIM_CUE_PICKUP), true, 'the undone cue\'s file is what it waits on');
        await fake.drain(() => filesOf(ANIM_CUE_PICKUP).every((f) => engine.loaded(f)));
        assert.equal(fake.sources.length, 1);
        const backBuf = fake.sources[0].buffer as any;
        const file = backBuf.url === '' ? null : backBuf.url;
        assert.equal(file, null, 'it plays a buffer made by reversing, not a decoded file');
        assert.ok(backBuf.data[0] > backBuf.data[3], 'the samples run backwards');
    } finally { fake.restore(); }
});

test('a revert that undoes nothing, or a cue with no sound, is silent', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        engine.unlock();
        await fake.settle();
        assert.equal(engine.strike(ANIM_CUE_REVERT), false, 'no cue named: nothing to play backwards');
        assert.equal(engine.strike(ANIM_CUE_REVERT, 0, 1, ANIM_CUE_REVERT), false, 'a revert of a revert is not a sound');
        assert.equal(fake.sources.length, 0);
    } finally { fake.restore(); }
});

// ---- 4. the table ---------------------------------------------------------------------

test('every cue the kernel can make has a row in the table', () => {
    assert.ok(!(ANIM_CUE_REVERT in CUE_SOUNDS),
        'REVERT has a row. It has no sound of its own: a refused move is the undone cue\'s file played backwards, '
        + 'so a row here would be a second sound to keep in step with the first.');
    for (let kind = 1; kind < ANIM_N_CUES; kind++) {
        if (kind === ANIM_CUE_REVERT) continue;
        assert.ok(kind in CUE_SOUNDS, `ANIM_CUE_${ANIM_CUE_NAMES[kind]} (${kind}) has no row in src/audio/cueSounds.ts. `
            + 'A cue that is meant to be silent gets `null`; one that is forgotten gets this.');
        const s = CUE_SOUNDS[kind];
        if (!s) continue;
        assert.ok(s.takes.length >= 1 && s.gain > 0 && s.gain <= 1 && s.pitch >= 0 && s.pitch < 0.2 && s.level >= 0 && s.level < 0.5,
            `${ANIM_CUE_NAMES[kind]}: a take, a gain in (0, 1], and wander small enough to still be the same sound`);
    }
    assert.ok(!(ANIM_CUE_NONE in CUE_SOUNDS), 'NONE is not a cue');
    const extra = Object.keys(CUE_SOUNDS).map(Number).filter((k) => !(k >= 1 && k < ANIM_N_CUES));
    assert.deepEqual(extra, [], 'and no row names a cue the kernel does not have');
});

test('a placeholder a row names is one soundgen writes, under the cue\'s own name; a recording is a committed file', () => {
    // The generator's own list of what it writes: name, cue id, rate, ms, bytes.
    const listing = execFileSync('make', ['-s', '--no-print-directory', '-C', join(REPO, 'tools/soundgen'), 'list'], { encoding: 'utf8' });
    const rows = listing.trim().split('\n').map((l) => l.split('\t'));
    const written = new Map(rows.map(([name, cue]) => [`gen/${name}`, Number(cue)]));
    const anchors = new Map(rows.map(([name, , , , , anchor]) => [`gen/${name}`, Number(anchor)]));
    const roles = new Map(rows.map(([name, , , , , , role]) => [`gen/${name}`, role]));
    assert.ok(written.size >= ANIM_N_CUES - 2, `soundgen lists its files (${written.size})`);
    assert.ok(![...written.keys()].some((f) => f.includes('revert')), 'soundgen writes no revert: there is no such sound');
    const namedByTable = new Set<string>();
    for (let kind = 1; kind < ANIM_N_CUES; kind++) {
        for (const { file, anchorMs, role } of CUE_SOUNDS[kind]?.takes ?? []) {
            namedByTable.add(file);
            if (file.startsWith('gen/')) {
                assert.equal(written.get(file), kind,
                    `${file} is named for ${ANIM_CUE_NAMES[kind]} but soundgen ${written.has(file) ? `writes it for cue ${written.get(file)}` : 'does not write it'}`);
                assert.ok(file.startsWith(`gen/${ANIM_CUE_NAMES[kind].toLowerCase()}_`), `${file} carries its cue's name`);
                assert.equal(anchorMs, anchors.get(file), `${file}: the table's anchor is the one soundgen wrote it with`);
                assert.equal(role, roles.get(file), `${file}: and its role`);
            } else {
                assert.ok(existsSync(join(REPO, 'public/audio', file)), `${file} is a recording, so it is committed under public/audio/`);
            }
        }
    }
    // The other direction: a recipe nobody plays is bytes every player downloads... except
    // that nothing downloads a file no row names. It is still dead weight in the tool.
    const orphans = [...written.keys()].filter((f) => !namedByTable.has(f));
    assert.deepEqual(orphans, [], 'soundgen writes a file no row names: its cue plays a recording now, so drop the recipe (make it NULL)');
});
