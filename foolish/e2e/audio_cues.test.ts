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
 *      starts what the kernel answers a strike plays (c/src/cue_voice.h, held
 *      natively in c/tests/anim_plan_test.c) on the audio clock, and every way
 *      it is meant to be silent is silent rather than broken: no Web Audio,
 *      muted, hidden, a file missing.
 *   4. THE TABLE - the kernel's (soundgen's, linked into web.wasm): every cue the
 *      kernel can make has takes in it, and it is the list soundgen writes.
 *
 * What the real page hands the engine during real moves is held in
 * e2e/ui_animation_trace.test.ts, where the page is.
 *
 * Pure kernel/wasm test - needs no Postgres.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

import {
    animBuildPlan, animCuesNext, animFinishCue, animRoleCue, cueLeadUs, cueMix, cueSeed, cueStrike, cueTakes,
    ANIM_EVT, ANIM_LOC, type AnimPlanEventIn, type CueStrike, type CueTakeRow,
} from '../sdk/ts/wasm/bots.ts';
import { ANIM_TIME_MS, ANIM_GAP_MS, ANIM_BOUT_END_HOLD_MS } from '../sdk/ts/gen/anim.bots.ts';
import { CARD_NONE_SUIT, CARD_NONE_VALUE, GAME_STATUS_PLAYING, PLAYER_STATUS_IN, type TableView_Snap as TableView } from '../sdk/ts/gen/view_layout.bots.ts';
import {
    ANIM_CUE_NONE, ANIM_CUE_DEAL, ANIM_CUE_LIFT, ANIM_CUE_PLACE, ANIM_CUE_COVER, ANIM_CUE_SWEEP,
    ANIM_CUE_PILE, ANIM_CUE_GOOD, ANIM_CUE_HAND_OFF, ANIM_CUE_FINISH, ANIM_CUE_FOOL, ANIM_CUE_OUT,
    ANIM_CUE_NAMES, ANIM_N_CUES, ANIM_SOUND_STAGGER_MS, ANIM_SOUND_OUT_LAG_MS,
    ANIM_CUE_FLIP, ANIM_CUE_PICKUP, ANIM_CUE_REVERT, ANIM_CUE_DEAL_LAND, ANIM_CUE_PASS, ANIM_CUE_TURN,
} from '../sdk/ts/gen/kernel_consts.bots.ts';
import { CUE_ROLE_PRELUDE, EVW_MSG_ATTACKED, EVW_MSG_PASSED } from '../sdk/ts/gen/view_layout.bots.ts';
import { ANIM_DECK_DOWN_MS } from '../sdk/ts/gen/anim.bots.ts';
import { CUE_LATE_MS, CUE_LOOK_AHEAD_MS, cueKey, dueStrikes, type PlanCue } from '../src/audio/cueSchedule.ts';
import { CueAudio, CUE_SOUND_ROOT, cueFile } from '../src/audio/cueAudio.ts';
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
        `OUT@0 step2 x1 (0)`,
        `SWEEP@${sweepAt} step3 x1 (4)`,
        `PILE@${sweepAt + ANIM_TIME_MS} step3 x1 (4)`,
        `DEAL@${dealAt} step4 x1 (3)`,
        `DEAL_LAND@${dealAt + ANIM_TIME_MS} step4 x1 (3)`,
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
    assert.equal(struck.length, 10, 'a lift, two covers, an out, the sweep, the pile, four dealt cards and their landings (a refill of three is ONE pack off the stock and ONE landing)');
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
    // The real table's leads (the kernel's, cue_lead_us): the deal's thumb slide and grab, the throw-in's.
    const cueLeadMs = (kind: number) => cueLeadUs(kind) / 1000;
    assert.ok(cueLeadMs(ANIM_CUE_DEAL) > 150 && cueLeadMs(ANIM_CUE_LIFT) > 150, 'the deal and a lift start well before the card leaves');
    // ...and the scrape of a sweep or a pickup begins ON sound, with the cards: cropped where it is
    // first audible, its file starts at the moment (a few ms of anchor), never a quiet lead-in.
    const scrapes = cueTakes().filter((t) => t.role === CUE_ROLE_PRELUDE);
    assert.ok(scrapes.length > 0 && scrapes.every((t) => t.anchorUs < 60_000),
        'the scrape starts with the cards moving: no take of it has a lead-in over 60 ms');
    assert.ok(cueLeadMs(ANIM_CUE_PASS) <= 0, 'the slide on starts after its landing: no lead');
});

// A SCRAPE IS HEARD AS THE CARDS MOVE (Alex: "just crop the scrape so that it's audible at 0"): every
// take of the strokes before a sweep or a pickup starts on sound, not on the quiet swell a stroke
// begins with.
test('every scrape take starts audible: its first 20 ms are not the quiet swell of a stroke', () => {
    const dir = mkdtempSync(join(tmpdir(), 'soundgen-scrape-'));
    try {
        const SG = join(REPO, 'tools/soundgen');
        execFileSync(join(SG, 'build/soundgen'), ['write', dir, join(dir, 'ct.c'), join(dir, 'tb.bin')],
            { cwd: SG, stdio: 'ignore' });
        const names = readdirSync(dir).filter((f) => /^(sweep|pickup)_strokes_s\d_\d+\.wav$/.test(f));
        assert.ok(names.length >= 10, 'the scrape takes of a sweep and a pickup are there');
        for (const f of names) {
            const b = readFileSync(join(dir, f));
            const rate = b.readUInt32LE(24), n = (b.length - 44) >> 1;
            const at = (i: number) => Math.abs(b.readInt16LE(44 + 2 * i));
            let peak = 0, first = 0;
            for (let i = 0; i < n; i++) peak = Math.max(peak, at(i));
            for (let i = 0; i < Math.floor(rate * 0.02); i++) first = Math.max(first, at(i));
            assert.ok(first >= 0.04 * peak, `${f}: audible from its first 20 ms (${first} of a peak of ${peak})`);
        }
    } finally { rmSync(dir, { recursive: true, force: true }); }
});

test('a tab that was away does not replay what it missed', () => {
    const plan = planOf(boutEnd());
    // The loop ran for 200ms, then nothing for five seconds (a hidden tab gets
    // no frames), then it comes back to a run that is long over.
    const done = new Set<number>();
    const before = [0, 16, 32, 200].flatMap((now) => [...dueStrikes(plan.cues, now, done)]);
    assert.deepEqual(before.map((s) => s.kind).sort(), [ANIM_CUE_LIFT, ANIM_CUE_OUT].sort(),
        'only the lift and the out (struck as the seat releases its last hand) were due in the first 200ms');
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
//
// What a strike PLAYS is the kernel's answer (c/src/cue_voice.h, held natively by
// c/tests/anim_plan_test.c test_cue_voice, and to the web engine's own recorded
// decisions by test_cue_voice_matches_the_web). What is the engine's is starting
// that answer on the audio clock, and every way of being silent.

interface FakeSource { buffer: unknown; playbackRate: { value: number }; startedAt: number | null; offset: number; stopped: boolean; gain: number; onended: (() => void) | null }
/** The kernel's take a source plays (its buffer names its file; a reversed buffer names none). */
const takeOf = (s: FakeSource): CueTakeRow | null => {
    const file = (s.buffer as { url: string }).url;
    return cueTakes().find((t) => cueFile(t.name) === file) ?? null;
};
/** Every file the kernel's table names for these cues, in the table's order. */
const filesOf = (...kinds: number[]) => cueTakes().filter((t) => kinds.includes(t.cue)).map((t) => cueFile(t.name));

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
        close() { this.state = 'closed'; return Promise.resolve(); }
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
            : { ok: true, status: 200, arrayBuffer: () => Promise.resolve(new TextEncoder().encode(url.replace(CUE_SOUND_ROOT, '')).buffer) }; };
        if (!opts.hold) return Promise.resolve().then(answer);
        return new Promise((ok) => { held.push({ url, go: () => ok(answer()) }); });
    };
    g.localStorage = { getItem: (k: string) => storage.get(k) ?? null, setItem: (k: string, v: string) => { storage.set(k, v); } };
    g.document = { hidden: false };
    return {
        sources, fetched, storage, ctxs, created: () => created, doc: g.document as { hidden: boolean },
        widest: () => widest, waiting: () => held.map((h) => h.url.replace(CUE_SOUND_ROOT, '')),
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
        restore: () => { Object.assign(g, saved); if (saved.AudioContext === undefined) delete g.AudioContext; delete g.__foolishCueContext; },
    };
}

// THE VOICE IS THE KERNEL'S, one a module: seeding it just before the engine strikes
// and again just before the same question is put to the kernel directly puts both
// in the same state, so what the engine started can be held to the answer.
const SEED = 0x5EED;
const kernelSays = (kind: number, delayMs: number, nCards = 1, undoes = 0, gainPm = 1000): CueStrike => {
    cueSeed(SEED);
    return cueStrike(kind, Math.round(Math.max(0, delayMs) * 1000), nCards, undoes, gainPm);
};
const strikeSeeded = (engine: CueAudio, kind: number, delayMs: number, nCards = 1, undoes = 0, gainPm = 1000): boolean => {
    cueSeed(SEED);
    return engine.strike(kind, delayMs, nCards, undoes, gainPm);
};
/** An engine with its context open and every file decoded, which has seeded its
 *  voice already (it does that on its first strike, from Math.random). */
async function ready(fake: ReturnType<typeof fakeAudio>): Promise<CueAudio> {
    const engine = new CueAudio();
    engine.unlock();
    await fake.settle();
    assert.ok(engine.strike(ANIM_CUE_PLACE));
    fake.sources.length = 0;
    return engine;
}

test('one audio context a page: an engine made again closes the one before it', async () => {
    // A hot update in development makes the engine again in the same page. Left running,
    // the old contexts wedged Safari's audio (seconds late, then stuttering, then silent).
    const fake = fakeAudio();
    try {
        const first = new CueAudio();
        first.unlock();
        const second = new CueAudio();
        second.unlock();
        assert.equal(fake.created(), 2);
        assert.equal(fake.ctxs[0].state, 'closed', 'the first engine\'s context is closed');
        assert.equal(fake.ctxs[1].state, 'running', 'the new one plays');
    } finally {
        fake.restore();
    }
});

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
        assert.equal(engine.strike(ANIM_CUE_PLACE), false, 'before a gesture there is no context and nothing plays');
        assert.equal(fake.created(), 0);
        engine.unlock();
        assert.equal(fake.created(), 1);
        await fake.settle();
        assert.ok(engine.strike(ANIM_CUE_PLACE), 'its first strike seeds its voice, from Math.random');
        assert.equal(strikeSeeded(engine, ANIM_CUE_PLACE, 120), true);
        const s = fake.sources[fake.sources.length - 1];
        const k = kernelSays(ANIM_CUE_PLACE, 120);
        const take = takeOf(s)!;
        assert.equal(take.name, cueTakes()[k.take].name, 'the take the kernel chose');
        assert.ok(take.anchorUs > 0 && take.anchorUs < 120000, 'a landing\'s take has its peak a little way in');
        assert.equal(k.startUs, 120000 - take.anchorUs, 'which the kernel starts its anchor before the moment');
        assert.ok(Math.abs(s.startedAt! - (10 + k.startUs / 1e6)) < 1e-9, 'and the engine starts there on the audio clock');
        assert.equal(s.offset, 0, 'from its first sample');
        assert.equal(s.playbackRate.value, 1);
        assert.equal(s.gain, k.gainPm / 1000, 'at the level the kernel gives it');
        engine.unlock();
        assert.equal(fake.created(), 1, 'a second gesture does not make a second context');
    } finally { fake.restore(); }
});

test('every file the kernel\'s table names is fetched once, from /audio/gen/', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        engine.unlock();
        await fake.settle();
        const files = cueTakes().map((t) => cueFile(t.name));
        assert.deepEqual([...fake.fetched].sort(), files.map((f) => `/audio/gen/${f}`).sort());
        for (const f of files) assert.ok(engine.loaded(f), `${f} decoded`);
    } finally { fake.restore(); }
});

test('the engine starts exactly what the kernel answers, take and strokes, for every kind of strike', async () => {
    const fake = fakeAudio();
    try {
        const engine = await ready(fake);
        let n = 0, preludes = 0, reversed = 0, wandered = 0;
        for (let kind = 1; kind < ANIM_N_CUES; kind++) {
            for (const undoes of kind === ANIM_CUE_REVERT ? [ANIM_CUE_PLACE, ANIM_CUE_COVER, ANIM_CUE_PICKUP] : [0]) {
                for (const delayMs of [0, 100, 300, 1000, 5000]) {
                    for (const nCards of [1, 10]) {
                        for (const gainPm of [1000, 455]) {
                            const before = fake.sources.length;
                            const ok = strikeSeeded(engine, kind, delayMs, nCards, undoes, gainPm);
                            const k = kernelSays(kind, delayMs, nCards, undoes, gainPm);
                            const made = fake.sources.slice(before);
                            const want = [k.take, k.prelude].filter((t) => t >= 0);
                            const what = `${ANIM_CUE_NAMES[kind]}${undoes ? `<${ANIM_CUE_NAMES[undoes]}` : ''} in ${delayMs} ms, ${nCards} cards, ${gainPm}`;
                            assert.equal(ok, k.take >= 0, `${what}: struck exactly when the kernel names a take`);
                            assert.equal(made.length, want.length, `${what}: a source for the take and one for its strokes`);
                            const plays = [
                                { take: k.take, startUs: k.startUs, gainPm: k.gainPm, ratePm: k.ratePm, backwards: k.backwards === 1 },
                                { take: k.prelude, startUs: k.preludeStartUs, gainPm: k.preludeGainPm, ratePm: k.preludeRatePm, backwards: false },
                            ];
                            made.forEach((s, i) => {
                                const p = plays[i];
                                const file = (s.buffer as { url: string }).url;
                                if (p.backwards) assert.equal(file, '', `${what}: a reversed buffer, made by the engine`);
                                else assert.equal(file, cueFile(cueTakes()[p.take].name), `${what}: the kernel's take`);
                                assert.ok(Math.abs(s.startedAt! - (10 + p.startUs / 1e6)) < 1e-9, `${what}: started when the kernel says`);
                                assert.equal(s.gain, p.gainPm / 1000, `${what}: at its level`);
                                assert.equal(s.playbackRate.value, p.ratePm / 1000, `${what}: at its speed`);
                                assert.equal(s.offset, 0, `${what}: whole`);
                                n++; if (i === 1) preludes++; if (p.backwards) reversed++; if (p.ratePm !== 1000) wandered++;
                            });
                        }
                    }
                }
            }
        }
        assert.ok(n > 300 && preludes > 20 && reversed > 20 && wandered > 5,
            `every way a strike can play was struck (${n} sources, ${preludes} strokes, ${reversed} reversed, ${wandered} off speed)`);
    } finally { fake.restore(); }
});

test('two files are two takes: the same one is never played twice running', async () => {
    const fake = fakeAudio();
    try {
        const engine = await ready(fake);
        assert.ok(filesOf(ANIM_CUE_DEAL).length >= 2, 'the deal has several takes, which is the case under test');
        let last: unknown = null;
        for (let i = 0; i < 12; i++) {
            assert.ok(engine.strike(ANIM_CUE_DEAL, 1000));
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
        const engine = await ready(fake);
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
        const engine = await ready(fake);
        fake.doc.hidden = true;
        assert.equal(engine.strike(ANIM_CUE_PLACE), false);
        assert.equal(fake.sources.length, 0, 'nothing was even scheduled');
        fake.doc.hidden = false;
        assert.equal(engine.strike(ANIM_CUE_PLACE), true);
    } finally { fake.restore(); }
});

test('a context the browser has suspended makes no sound until a gesture wakes it', async () => {
    const fake = fakeAudio();
    try {
        const engine = await ready(fake);
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

const short = (urls: string[]) => urls.map((u) => u.replace(CUE_SOUND_ROOT, ''));
const ALL_FILES = cueTakes().map((t) => cueFile(t.name));

test('before a gesture only the wanted cues are fetched, as bytes, and nothing at all unasked', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        await fake.settle();
        assert.deepEqual(fake.fetched, [], 'an engine nobody has told anything fetches nothing');
        engine.want(bits(ANIM_CUE_DEAL, ANIM_CUE_FLIP));
        await fake.settle();
        const lobby = filesOf(ANIM_CUE_DEAL, ANIM_CUE_FLIP);
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
        const wantedFiles = filesOf(ANIM_CUE_PICKUP, ANIM_CUE_COVER);
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
        assert.equal(short(fake.fetched)[3], 'finish_0.m4a', 'the next place goes to a wanted file, far ahead of it in the table');
        await fake.release(1);
        assert.equal(short(fake.fetched)[4], 'fool_0.m4a');
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
        assert.equal(short(fake.fetched)[3], 'fool_0.m4a', 'a file at the end of the table is the next one fetched');
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

test('a revert plays the undone cue\'s own file reversed, where the kernel says, and reverses it once', async () => {
    const fake = fakeAudio();
    try {
        const engine = await ready(fake);
        const k = kernelSays(ANIM_CUE_REVERT, 1500, 1, ANIM_CUE_PICKUP);
        const take = cueTakes()[k.take];
        assert.ok(k.backwards === 1 && take.cue === ANIM_CUE_PICKUP, 'the kernel answers one of the pickup\'s own takes, backwards');
        const lenUs = take.nSamples * 1e6 / take.rate;
        assert.ok(Math.abs(k.startUs - (1500000 - (lenUs - take.anchorUs))) <= 1,
            'its anchor measured from the file\'s other end: the reversed take peaks on the revert\'s moment');
        const forwards = (engine as any).buffers.get(cueFile(take.name));
        const was = [...forwards.data];
        assert.ok(strikeSeeded(engine, ANIM_CUE_REVERT, 1500, 1, ANIM_CUE_PICKUP));
        const back = fake.sources[0];
        assert.notEqual(back.buffer, forwards, 'not the same buffer');
        assert.deepEqual([...(back.buffer as any).data], [...was].reverse(), 'the same samples, last first');
        assert.equal(back.gain, k.gainPm / 1000, 'at the level the kernel gives it');
        assert.ok(Math.abs(back.startedAt! - (10 + k.startUs / 1e6)) < 1e-9, 'and started where it says');
        assert.deepEqual([...forwards.data], was, 'the forward buffer is not disturbed');
        assert.ok(strikeSeeded(engine, ANIM_CUE_REVERT, 0, 1, ANIM_CUE_PICKUP));
        assert.equal(fake.sources[1].buffer, back.buffer, 'the reversed buffer is made once and kept');
        assert.ok(engine.strike(ANIM_CUE_PICKUP));
        assert.notEqual((fake.sources[2].buffer as any).url, '', 'and the cue itself still plays forwards, off its own file');
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
        const engine = await ready(fake);
        assert.equal(engine.strike(ANIM_CUE_REVERT), false, 'no cue named: nothing to play backwards');
        assert.equal(engine.strike(ANIM_CUE_REVERT, 0, 1, ANIM_CUE_REVERT), false, 'a revert of a revert is not a sound');
        assert.equal(engine.strike(ANIM_N_CUES), false, 'nor is a cue the kernel does not have');
        assert.equal(fake.sources.length, 0);
    } finally { fake.restore(); }
});

// ---- 4. the table ---------------------------------------------------------------------

test('every cue the kernel can make has takes in its table and a level, and a revert has none', () => {
    assert.ok(!cueTakes().some((t) => t.cue === ANIM_CUE_REVERT),
        'REVERT has takes. It has no sound of its own: a refused move is the undone cue\'s file played backwards, '
        + 'so a take of its own would be a second sound to keep in step with the first.');
    assert.ok(!cueTakes().some((t) => t.cue === ANIM_CUE_NONE || t.cue >= ANIM_N_CUES), 'and no take is for a cue the kernel does not have');
    for (let kind = 1; kind < ANIM_N_CUES; kind++) {
        if (kind === ANIM_CUE_REVERT) continue;
        const m = cueMix(kind)!;
        assert.ok(cueTakes().some((t) => t.cue === kind), `ANIM_CUE_${ANIM_CUE_NAMES[kind]} (${kind}) has no take in soundgen's table`);
        assert.ok(m.gainPm > 0 && m.gainPm <= 1000 && m.pitchPm >= 0 && m.pitchPm < 200 && m.levelPm >= 0 && m.levelPm < 500,
            `${ANIM_CUE_NAMES[kind]}: a gain in (0, 1000], and wander small enough to still be the same sound`);
    }
    assert.equal(cueMix(ANIM_CUE_NONE), null, 'NONE is not a cue');
});

// THE STOCK COMES DOWN WITH ITS SOUND, and the moment is the plan's (ANIM_DECK_DOWN_MS, c/src/anim_plan.h):
// the "stock set down on it" in the trump's take is made to sit there, so tuning.tsv's time for that
// part is overruled and nobody twiddles a delay to line the sound up with the picture.
test('the stock set down is heard at the plan\'s moment, whatever the file says its time is', () => {
    const dir = mkdtempSync(join(tmpdir(), 'soundgen-deck-'));
    try {
        const SG = join(REPO, 'tools/soundgen');
        const tuning = readFileSync(join(SG, 'tuning.tsv'), 'utf8');
        const line = /^(part\tflip\tmain\tdeck\t)(-?[\d.]+)(\t)/m;
        assert.ok(line.test(tuning), 'the stock set down has a line in the file');
        const table = (text: string, name: string) => {
            const path = join(dir, `${name}.tsv`), out = join(dir, `${name}.c`);
            writeFileSync(path, text);
            execFileSync('make', ['-s', '--no-print-directory', '-C', SG, 'ctable', `CTABLE=${out}`],
                { env: { ...process.env, SOUNDGEN_TUNING: path }, stdio: 'ignore' });
            return readFileSync(out, 'utf8');
        };
        const same = table(tuning, 'as-is');
        assert.equal(table(tuning.replace(line, '$1900$3'), 'later'), same, 'a later time in the file changes no take');
        assert.equal(table(tuning.replace(line, '$1-40$3'), 'earlier'), same, 'and nor does an earlier one');
        assert.equal(ANIM_DECK_DOWN_MS, 250, 'the plan\'s moment, from the kernel\'s header');
    } finally { rmSync(dir, { recursive: true, force: true }); }
});

test('the kernel plays by the table soundgen writes its files from', () => {
    // The generator's own list of what it writes: name, cue, rate, ms, bytes, anchor, role, level.
    const listing = execFileSync('make', ['-s', '--no-print-directory', '-C', join(REPO, 'tools/soundgen'), 'list'], { encoding: 'utf8' });
    const rows = listing.trim().split('\n').map((l) => l.split('\t'));
    const table = cueTakes();
    assert.ok(rows.length >= ANIM_N_CUES - 2, `soundgen lists its files (${rows.length})`);
    assert.deepEqual(table.map((t) => cueFile(t.name)), rows.map(([name]) => name),
        'every file soundgen writes is a take of the kernel\'s table, in its order, and no other');
    assert.ok(!rows.some(([name]) => name.includes('revert')), 'soundgen writes no revert: there is no such sound');
    const ROLES = ['main', 'tight', 'prelude'];
    rows.forEach(([name, cue, rate, , , anchor, role, level], i) => {
        const t = table[i];
        assert.equal(t.cue, Number(cue), `${name}: its cue`);
        assert.ok(name.startsWith(`${ANIM_CUE_NAMES[t.cue].toLowerCase()}_`), `${name} carries its cue's name`);
        assert.equal(t.rate, Number(rate), `${name}: its rate`);
        assert.ok(Math.abs(t.anchorUs / 1000 - Number(anchor)) <= 0.0501, `${name}: the anchor soundgen wrote it with (the list rounds it to a tenth of a millisecond)`);
        assert.equal(ROLES[t.role], role, `${name}: its role`);
        assert.equal(t.level, Number(level), `${name}: its stroke count`);
    });
});

// A RECORDING ENTERS WHERE A MODEL DOES: as a take soundgen lists (tools/soundgen
// recordings/recordings.tsv), so it is in the table both hosts play by and among
// the files they serve and bundle, with nothing in any host to edit.
test('a recording listed for soundgen is a take of its cue in the table every host plays by, and replaces its models', () => {
    const dir = mkdtempSync(join(tmpdir(), 'soundgen-rec-'));
    try {
        // 3200 samples at 32 kHz of a 16-bit mono PCM WAV: the 44-byte header and the data.
        const n = 3200, rate = 32000;
        const wav = Buffer.alloc(44 + 2 * n);
        wav.write('RIFF', 0); wav.writeUInt32LE(36 + 2 * n, 4); wav.write('WAVEfmt ', 8);
        wav.writeUInt32LE(16, 16); wav.writeUInt16LE(1, 20); wav.writeUInt16LE(1, 22); wav.writeUInt32LE(rate, 24);
        wav.writeUInt32LE(rate * 2, 28); wav.writeUInt16LE(2, 32); wav.writeUInt16LE(16, 34);
        wav.write('data', 36); wav.writeUInt32LE(2 * n, 40);
        for (let k = 0; k < n; k++) wav.writeInt16LE(Math.round(8000 * Math.sin(k / 9) * Math.exp(-k / 600)), 44 + 2 * k);
        writeFileSync(join(dir, 'place_rec_0.wav'), wav);
        writeFileSync(join(dir, 'recordings.tsv'), '# cue role level file anchor_ms kbps\nplace main 0 place_rec_0.wav 12.5 96\n');
        const out = join(dir, 'cue_takes.c');
        const SG = join(REPO, 'tools/soundgen');
        execFileSync('make', ['-s', '--no-print-directory', '-C', SG, 'ctable', `CTABLE=${out}`],
            { env: { ...process.env, SOUNDGEN_RECORDINGS: dir }, stdio: ['ignore', 'ignore', 'inherit'] });
        const rows = [...readFileSync(out, 'utf8').matchAll(/\{ (-?\d+), (-?\d+), (-?\d+), (\d+), (\d+), (\d+), \d+, "([^"]+)" \}/g)]
            .map((m) => ({ rate: +m[1], n: +m[2], anchorUs: +m[3], cue: +m[4], role: +m[5], level: +m[6], name: m[7] }));
        assert.deepEqual(rows.filter((r) => r.cue === ANIM_CUE_PLACE),
            [{ rate, n, anchorUs: 12500, cue: ANIM_CUE_PLACE, role: 0, level: 0, name: 'place_rec_0' }],
            'the recording is the throw-in landing\'s one take, at its own rate and length, with the anchor it was listed with');
        assert.ok(rows.some((r) => r.cue === ANIM_CUE_COVER) && rows.length > 100, 'and every other cue keeps its models');
        // A line soundgen cannot read stops the build rather than dropping a sound.
        writeFileSync(join(dir, 'cover_rec_0.wav'), wav);
        writeFileSync(join(dir, 'recordings.tsv'), 'place main 0 cover_rec_0.wav 12.5 96\n');
        assert.throws(() => execFileSync('make', ['-s', '--no-print-directory', '-C', SG, 'ctable', `CTABLE=${out}`],
            { env: { ...process.env, SOUNDGEN_RECORDINGS: dir }, stdio: 'ignore' }), 'a recording named for another cue is refused');
    } finally { rmSync(dir, { recursive: true, force: true }); }
});

// THE TIMING AND LEVEL OF EVERY PART ARE tools/soundgen/tuning.tsv's: the tuner on
// /sounds edits that file and nothing else, so a part moved there is a take that
// starts that much earlier, and a file soundgen cannot read in full stops the build.
test('tuning.tsv places every part: the deal\'s thumb slide moved 100 ms earlier is every deal take\'s anchor 100 ms longer, and nothing else moves', () => {
    const dir = mkdtempSync(join(tmpdir(), 'soundgen-tune-'));
    try {
        const SG = join(REPO, 'tools/soundgen');
        const tuning = readFileSync(join(SG, 'tuning.tsv'), 'utf8');
        const ctable = (text: string, name: string) => {
            const path = join(dir, `${name}.tsv`), out = join(dir, `${name}.c`);
            writeFileSync(path, text);
            execFileSync('make', ['-s', '--no-print-directory', '-C', SG, 'ctable', `CTABLE=${out}`],
                { env: { ...process.env, SOUNDGEN_TUNING: path }, stdio: 'ignore' });
            return [...readFileSync(out, 'utf8').matchAll(/\{ (-?\d+), (-?\d+), (-?\d+), (\d+), (\d+), (\d+), \d+, "([^"]+)" \}/g)]
                .map((m) => ({ n: +m[2], anchorUs: +m[3], cue: +m[4], role: +m[5], name: m[7] }));
        };
        // Wherever it is tuned to: the test moves it, it does not pin it.
        const line = /^part\tdeal\tmain\tthumb\t(-?\d+)\t/m;
        const at = Number(line.exec(tuning)?.[1]);
        assert.ok(Number.isFinite(at), 'the deal\'s thumb slide has a line in the file');
        const before = ctable(tuning, 'before');
        const after = ctable(tuning.replace(line, `part\tdeal\tmain\tthumb\t${at - 100}\t`), 'after');
        const deal = (r: { cue: number; role: number }) => r.cue === ANIM_CUE_DEAL && r.role === 0;
        assert.equal(before.filter(deal).length, 8, 'eight ordinary deal takes');
        before.forEach((b, i) => {
            const a = after[i];
            if (deal(b)) {
                assert.equal(a.anchorUs - b.anchorUs, 100000, `${b.name}: its moment 100 ms further into the take`);
                assert.equal(a.n - b.n, 4800, `${b.name}: and the take 100 ms longer (48 kHz)`);
            } else {
                assert.deepEqual(a, b, `${b.name}: not a part of it, so as it was`);
            }
        });
        for (const [bad, why] of [
            [tuning.replace(/^part\tpass\t.*\n/m, ''), 'a part with no line'],
            [tuning.replace(/^(part\tpass\t.*\n)/m, '$1$1'), 'a part with two'],
            [tuning.replace(/^part\tpass\tmain\tslide\t/m, 'part\tpass\tmain\tslid\t'), 'a part soundgen does not have'],
            [tuning.replace(/^mix\tgood\t.*\n/m, ''), 'a cue with no mix line'],
        ] as const) {
            assert.notEqual(bad, tuning, `the test changed the file (${why})`);
            assert.throws(() => ctable(bad, 'bad'), `${why} stops the build`);
        }
    } finally { rmSync(dir, { recursive: true, force: true }); }
});

// ---- 5. the shipped files: AAC, starting on the millisecond ------------------------------

// An AAC encoder starts its stream with priming samples, and an anchor counts
// samples from a take's FIRST sample, so a file that decodes even a few samples
// late puts every sound that late. The edit list says where the take starts
// (tools/soundgen/c/m4a.h); every browser decodes by it (docs/AUDIO.md, "Shipped
// as AAC", measured in headless Chrome and Firefox and through Core Audio, which
// Safari decodes with). This holds the files to it: their containers say so to
// the sample (`make check`), and ffmpeg - Chrome's own decoder - hands back the
// WAV soundgen rendered, from its first sample.
test('every shipped file starts on its take\'s first sample', () => {
    const SG = join(REPO, 'tools/soundgen');
    const manifest = join(SG, 'build/wav/takes.tsv');
    assert.ok(existsSync(manifest), 'the sounds were built (npm run sounds) before this ran');
    const checked = execFileSync('make', ['-s', '--no-print-directory', '-C', SG, 'check'], { encoding: 'utf8' });
    assert.match(checked, / 0 bad;/, 'every file declares the encoder\'s priming and the take\'s length, to the sample');
    const pcm = (buf: Buffer, from: number) => { const v = new Int16Array(buf.buffer, buf.byteOffset + from, (buf.length - from) >> 1); return Float32Array.from(v, (x) => x / 32768); };
    const seen = new Set<string>();
    for (const line of readFileSync(manifest, 'utf8').trim().split('\n')) {
        const [name] = line.split(' ');
        const set = name.replace(/_\d+$/, '');
        if (seen.has(set)) continue;
        seen.add(set);
        const wav = pcm(readFileSync(join(SG, 'build/wav', `${name}.wav`)), 44);
        const dec = pcm(execFileSync('ffmpeg', ['-nostdin', '-loglevel', 'error', '-i', join(REPO, 'public/audio/gen', `${name}.m4a`), '-f', 's16le', '-'], { maxBuffer: 1 << 26 }), 0);
        assert.ok(dec.length >= wav.length, `${name}: the whole take decodes`);
        // The lag that lines the decoded take up with the rendered one, around its peak.
        let peak = 0;
        for (let i = 1; i < wav.length; i++) if (Math.abs(wav[i]) > Math.abs(wav[peak])) peak = i;
        const lo = Math.max(0, peak - 2048), hi = Math.min(wav.length, peak + 2048);
        let best = -Infinity, lag = 0;
        for (let L = -1100; L <= 1100; L++) {
            let c = 0;
            for (let i = lo; i < hi; i++) { const j = i + L; if (j >= 0 && j < dec.length) c += wav[i] * dec[j]; }
            if (c > best) { best = c; lag = L; }
        }
        assert.equal(lag, 0, `${name}: decoded ${lag} samples off its anchor`);
    }
    assert.ok(seen.size > 25, `one take of every set was decoded (${seen.size})`);
});
