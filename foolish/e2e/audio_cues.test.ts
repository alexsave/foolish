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
    animBuildPlan, animFinishCue, animRoleCue, ANIM_EVT, ANIM_LOC, type AnimPlanEventIn,
} from '../sdk/ts/wasm/bots.ts';
import { ANIM_TIME_MS, ANIM_GAP_MS, ANIM_BOUT_END_HOLD_MS } from '../sdk/ts/gen/anim.bots.ts';
import { CARD_NONE_SUIT, CARD_NONE_VALUE, GAME_STATUS_PLAYING, PLAYER_STATUS_IN, type TableView_Snap as TableView } from '../sdk/ts/gen/view_layout.bots.ts';
import {
    ANIM_CUE_NONE, ANIM_CUE_DEAL, ANIM_CUE_LIFT, ANIM_CUE_PLACE, ANIM_CUE_COVER, ANIM_CUE_SWEEP,
    ANIM_CUE_PILE, ANIM_CUE_GOOD, ANIM_CUE_HAND_OFF, ANIM_CUE_FINISH, ANIM_CUE_FOOL, ANIM_CUE_OUT,
    ANIM_CUE_NAMES, ANIM_N_CUES, ANIM_SOUND_STAGGER_MS, ANIM_SOUND_OUT_LAG_MS,
} from '../sdk/ts/gen/kernel_consts.bots.ts';
import { CUE_LATE_MS, CUE_LOOK_AHEAD_MS, cueKey, dueStrikes, type PlanCue } from '../src/audio/cueSchedule.ts';
import { CUE_SOUNDS } from '../src/audio/cueSounds.ts';
import { CueAudio } from '../src/audio/cueAudio.ts';
import { finishCueOf } from '../src/audio/useFinishCue.ts';

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
    deckCount: 12, discardPileLength: 0, hasFlipped: false, hasGoodTimestamp: false, passing: true,
    flipped: { suit: CARD_NONE_SUIT, value: CARD_NONE_VALUE }, goodMask: 0, version: 0, battles: [],
    seats: [0, 1].map(() => ({ status: PLAYER_STATUS_IN, handCount: 6, awaitingAttack: false, isAi: false, id: '', name: '' })),
    myHand: [], elimination: [], rated: false, ratingDelta: new Array(8).fill(0), ratingBefore: new Array(8).fill(0),
    gameId: 'g', title: '',
};
const planOf = (events: AnimPlanEventIn[]) => animBuildPlan(events, TABLE);
const named = (cues: readonly PlanCue[]) =>
    cues.map((c) => `${ANIM_CUE_NAMES[c.kind]}@${c.atMs} step${c.step} x${c.hits} (${c.nCards})`);

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
        `DEAL@${dealAt + ANIM_TIME_MS + ANIM_GAP_MS} step5 x1 (1)`,
    ]);
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
    assert.equal(animFinishCue(-1, 0), ANIM_CUE_NONE);
    assert.equal(animFinishCue(2, 2), ANIM_CUE_FOOL);
    assert.equal(animFinishCue(2, 0), ANIM_CUE_FINISH);
    assert.equal(animFinishCue(2, -1), ANIM_CUE_FINISH);
});

test('a board\'s finish cue reads the fool and the viewer off it, each as itself', () => {
    assert.equal(finishCueOf({ fool: -1, mySeat: 0 }), ANIM_CUE_NONE, 'a running game has no fool and makes no sound');
    assert.equal(finishCueOf({ fool: 1, mySeat: 1 }), ANIM_CUE_FOOL);
    assert.equal(finishCueOf({ fool: 1, mySeat: 0 }), ANIM_CUE_FINISH);
    // The one viewer for whom the two seats are not interchangeable: a spectator
    // has no seat, and still hears the game end.
    assert.equal(finishCueOf({ fool: 2, mySeat: -1 }), ANIM_CUE_FINISH);
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
    assert.equal(struck.length, 10, 'a lift, two covers, an out, the sweep, the pile, and four dealt cards (a refill of three is three strikes)');
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
    const cue: PlanCue = { atMs: 1000, kind: ANIM_CUE_SWEEP, step: 0, hits: 1, nCards: 7 };
    const done = new Set<number>();
    assert.deepEqual(dueStrikes([cue], 1000 - CUE_LOOK_AHEAD_MS - 1, done), []);
    assert.equal(done.size, 0, 'and it is not marked as handed over');
    assert.deepEqual(dueStrikes([cue], 1000 - CUE_LOOK_AHEAD_MS, done),
        [{ kind: ANIM_CUE_SWEEP, delayMs: CUE_LOOK_AHEAD_MS, nCards: 7 }],
        'with how far off it is and how many cards it stands for');
    assert.deepEqual(dueStrikes([cue], 990, done), [], 'once handed over it is never handed over again');
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
    const cue: PlanCue = { atMs: 500, kind: ANIM_CUE_PLACE, step: 3, hits: 3, nCards: 3 };
    // 500, 545, 590. At 500 + LATE + 1 the first is too old and the other two are not.
    const got = dueStrikes([cue], 500 + CUE_LATE_MS + 1, new Set());
    assert.equal(got.length, 2, 'the strike more than CUE_LATE_MS behind is dropped, its later siblings are not');
    assert.ok(got.every((s) => s.delayMs === 0 || s.delayMs > 0));
    assert.equal(dueStrikes([cue], 500 + CUE_LATE_MS, new Set()).length, 3, 'exactly CUE_LATE_MS behind still plays');
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

interface FakeSource { buffer: unknown; playbackRate: { value: number }; startedAt: number | null; stopped: boolean; gain: number; onended: (() => void) | null }

/** A stand-in AudioContext: a clock the test owns and a record of what was asked of it. */
function fakeAudio(opts: { state?: string; fail?: (url: string) => boolean } = {}) {
    const sources: FakeSource[] = [];
    const fetched: string[] = [];
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
                start(when: number) { s.startedAt = when; s.gain = s.gainNode.gain.value; },
                stop() { s.stopped = true; } };
            sources.push(s);
            return s;
        }
        decodeAudioData(bytes: ArrayBuffer, ok: (b: unknown) => void) { ok({ bytes: bytes.byteLength }); }
    }
    const g = globalThis as any;
    const saved = { AudioContext: g.AudioContext, fetch: g.fetch, localStorage: g.localStorage, document: g.document };
    const storage = new Map<string, string>();
    g.AudioContext = Ctx;
    g.fetch = (url: string) => {
        fetched.push(url);
        return Promise.resolve(opts.fail?.(url)
            ? { ok: false, status: 404, arrayBuffer: () => Promise.resolve(new ArrayBuffer(0)) }
            : { ok: true, status: 200, arrayBuffer: () => Promise.resolve(new ArrayBuffer(8)) });
    };
    g.localStorage = { getItem: (k: string) => storage.get(k) ?? null, setItem: (k: string, v: string) => { storage.set(k, v); } };
    g.document = { hidden: false };
    return {
        sources, fetched, storage, ctxs, created: () => created, doc: g.document as { hidden: boolean },
        /** Let every fetch and decode finish. */
        settle: async () => { for (let i = 0; i < 6; i++) await new Promise((r) => setImmediate(r)); },
        restore: () => { Object.assign(g, saved); if (saved.AudioContext === undefined) delete g.AudioContext; },
    };
}

test('with no Web Audio at all the engine is a no-op, not a crash', () => {
    const g = globalThis as any;
    assert.equal(g.AudioContext, undefined, 'Node has no AudioContext, which is the case under test');
    const engine = new CueAudio();
    engine.arm();
    engine.unlock();
    assert.equal(engine.strike(ANIM_CUE_PLACE), false);
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
        assert.equal(engine.strike(ANIM_CUE_PLACE), false, 'and before its file has decoded a cue is silent, not an error');
        await fake.settle();
        assert.equal(engine.strike(ANIM_CUE_PLACE, 120), true);
        const s = fake.sources[fake.sources.length - 1];
        assert.equal(s.startedAt, 10 + 0.12, 'currentTime + 120ms: scheduled, not played "now"');
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
        const files = Object.values(CUE_SOUNDS).flatMap((s) => s?.files ?? []);
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
        const place = CUE_SOUNDS[ANIM_CUE_PLACE]!;
        for (const r of [0, 0.999999]) {
            engine.random = () => r;
            assert.ok(engine.strike(ANIM_CUE_PLACE));
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
    } finally { fake.restore(); }
});

test('a cue that stands for many cards is heavier, up to a ceiling', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        engine.random = () => 0.5;
        engine.unlock();
        await fake.settle();
        const sweep = CUE_SOUNDS[ANIM_CUE_SWEEP]!;
        const gainFor = (n: number) => { engine.strike(ANIM_CUE_SWEEP, 0, n); return fake.sources[fake.sources.length - 1].gain; };
        assert.equal(gainFor(1), sweep.gain);
        assert.ok(Math.abs(gainFor(4) - sweep.gain * (1 + 3 * sweep.perCard)) < 1e-9, 'three cards past the first');
        assert.ok(Math.abs(gainFor(40) - sweep.gain * 1.6) < 1e-9, 'and never more than 60% over');
    } finally { fake.restore(); }
});

test('two files are two takes: the same one is never played twice running', async () => {
    const fake = fakeAudio();
    try {
        const engine = new CueAudio();
        engine.unlock();
        await fake.settle();
        assert.equal(CUE_SOUNDS[ANIM_CUE_DEAL]!.files.length, 2, 'the deal has two takes, which is the case under test');
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
        engine.strike(ANIM_CUE_SWEEP, 300);
        const s = fake.sources[fake.sources.length - 1];
        assert.equal(s.stopped, false);
        engine.setMuted(true);
        assert.equal(s.stopped, true, 'a sound scheduled 300ms out does not play after the player hit mute');
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

// ---- 4. the table ---------------------------------------------------------------------

test('every cue the kernel can make has a row in the table', () => {
    for (let kind = 1; kind < ANIM_N_CUES; kind++) {
        assert.ok(kind in CUE_SOUNDS, `ANIM_CUE_${ANIM_CUE_NAMES[kind]} (${kind}) has no row in src/audio/cueSounds.ts. `
            + 'A cue that is meant to be silent gets `null`; one that is forgotten gets this.');
        const s = CUE_SOUNDS[kind];
        if (!s) continue;
        assert.ok(s.files.length >= 1 && s.gain > 0 && s.gain <= 1 && s.pitch >= 0 && s.pitch < 0.2 && s.level >= 0 && s.level < 0.5,
            `${ANIM_CUE_NAMES[kind]}: a file, a gain in (0, 1], and wander small enough to still be the same sound`);
    }
    assert.ok(!(ANIM_CUE_NONE in CUE_SOUNDS), 'NONE is not a cue');
    const extra = Object.keys(CUE_SOUNDS).map(Number).filter((k) => !(k >= 1 && k < ANIM_N_CUES));
    assert.deepEqual(extra, [], 'and no row names a cue the kernel does not have');
});

test('a placeholder a row names is one soundgen writes, under the cue\'s own name; a recording is a committed file', () => {
    // The generator's own list of what it writes: name, cue id, rate, ms, bytes.
    const listing = execFileSync('make', ['-s', '--no-print-directory', '-C', join(REPO, 'tools/soundgen'), 'list'], { encoding: 'utf8' });
    const written = new Map(listing.trim().split('\n').map((l) => l.split('\t')).map(([name, cue]) => [`gen/${name}`, Number(cue)]));
    assert.ok(written.size >= ANIM_N_CUES - 1, `soundgen lists its files (${written.size})`);
    const namedByTable = new Set<string>();
    for (let kind = 1; kind < ANIM_N_CUES; kind++) {
        for (const file of CUE_SOUNDS[kind]?.files ?? []) {
            namedByTable.add(file);
            if (file.startsWith('gen/')) {
                assert.equal(written.get(file), kind,
                    `${file} is named for ${ANIM_CUE_NAMES[kind]} but soundgen ${written.has(file) ? `writes it for cue ${written.get(file)}` : 'does not write it'}`);
                assert.ok(file.startsWith(`gen/${ANIM_CUE_NAMES[kind].toLowerCase()}_`), `${file} carries its cue's name`);
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
