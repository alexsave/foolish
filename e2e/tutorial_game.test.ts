/* =============================================================================
 * The tutorial's frozen game still teaches the game (A5)
 * =============================================================================
 * The tutorial is a real game frozen as a replay code, and a replay code is only
 * readable by the kernel that cut it — the arithmetic coder's probability model
 * IS the legal-move menu, so a menu change renumbers every choice and orphans
 * the constant. That has happened twice in this repo, and the Oracle's fixture
 * sat dead for who knows how long. So the constant is checked here, on every
 * run, against the kernel that ships.
 *
 * Beyond "it decodes": the tutorial WAITS for the learner at their own steps. A
 * game that does not contain the moves it asks for does not fail — it hangs on a
 * board that never advances, in front of a beginner. So this walks the whole
 * thing the way the component does and insists it reaches the end.
 *
 * These run in the FAST validation suite as well as the full one
 * (registerTutorialValidation, same pattern as replay_codec.test.ts): a stranded
 * tutorial should fail in seconds, next to the note explaining how to re-cut it,
 * not in whatever ran the long sweep.
 * ========================================================================== */

import { test } from 'node:test';
import { bytesToBigint } from '../server/api/common/replay/codec.ts';
import { kernelB32Decode, replaySummary } from '../sdk/ts/wasm/bots.ts';
import assert from 'node:assert/strict';

import { bigintToBytes } from '../server/api/common/replay/codec.ts';
import { buildReplayFrames, REPLAY_STEP, ReplayFrame } from '../src/replay/frames.ts';
import { TUTORIAL_MOVES_CODE, TUTORIAL_NAMES } from '../src/components/tutorialGame.ts';
import { buildBeats } from '../src/components/tutorialBeats.ts';
import { PLAYER_STATUS } from '../src/state/view.ts';

// The ONE replay format: inline reveals, hidden-state-lossless, partial-game
// (c/src/replay.h REPLAY_FORMAT_VERSION_V10,
// docs/REPLAY_FORMAT6_HIDDEN_STATE.md). Was 6, then 7 (pass-mode bit), then 8
// (forced-opening bit), and is now 10 for a reason that is not a wire change at
// all: the bytes did not move, the deal order under them did. A code carrying
// any other version is refused, never re-read.
//
// A hand-written number on purpose: this is the pin. The codec itself is the C
// kernel's (c/src/replay.c, replay_steps.c) and hosts read a code through
// bots.wasm, so reading the version from the kernel here would assert the
// kernel against itself. Never change the wire format in one place: bump the
// version in replay.h AND decide what happens to every code already cut.
const FORMAT_VERSION_V6 = 10;

if (!process.env.E2E_VERBOSE) {
    console.log = () => {};
    console.warn = () => {};
    console.error = () => {};
}

const LEARNER = 0;

// Mirrors Tutorial.tsx's learnerOwesGood — a good that CLOSES a bout is not
// attributed to anyone (v6 records the round ending, not who ended it), so the
// learner's own closing good arrives as a seat-less ROUND_END.
const learnerOwesGood = (prev: ReplayFrame | undefined): boolean => {
    if (!prev) return false;
    const me = prev.game.seats[LEARNER];
    return !!me && me.status !== PLAYER_STATUS.OUT
        && prev.game.defender !== LEARNER
        && ((prev.game.goodMask >>> LEARNER) & 1) === 0;
};

const isLearnerStep = (frames: ReplayFrame[], i: number): boolean => {
    if (i < 0 || i >= frames.length) return false;
    const f = frames[i];
    if (f.kind === REPLAY_STEP.ROUND_END) return learnerOwesGood(frames[i - 1]);
    const moves: readonly number[] = [
        REPLAY_STEP.ATTACK, REPLAY_STEP.COVER, REPLAY_STEP.PASS,
        REPLAY_STEP.PICKUP, REPLAY_STEP.GOOD,
    ];
    return f.seat === LEARNER && moves.includes(f.kind);
};

// Read the way Tutorial.tsx reads it: the kernel's summary of the code, and the
// frames from the learner's seat.
const load = async () => {
    const x = bytesToBigint(kernelB32Decode(TUTORIAL_MOVES_CODE));
    const summary = replaySummary(bigintToBytes(x));
    assert.ok(summary, 'the tutorial code has a summary');
    const frames = buildReplayFrames(bigintToBytes(x), 'tutorial', TUTORIAL_NAMES, {
        viewer: LEARNER, fool: summary!.fool,
    });
    return { summary: summary!, frames };
};

export function registerTutorialValidation(): void {
test('the tutorial code still replays on the kernel that ships', async () => {
    const { summary, frames } = await load();
    assert.equal(summary.version, FORMAT_VERSION_V6,
        'the tutorial is an inline-reveal code (the retrodiction line cannot replay)');
    assert.equal(summary.numPlayers, 3, '3-player game');
    assert.ok(frames.length > 10, `replays to ${frames.length} steps`);
    assert.equal(frames[0].kind, REPLAY_STEP.DEAL, 'it opens with the deal');
    assert.notEqual(summary.fool, LEARNER, 'the learner is not left the fool');
    assert.equal(summary.firstAttacker, LEARNER, 'the learner holds the lowest trump and leads');
    assert.equal(summary.trump.suit, summary.powerSuit, 'the flipped trump names the trump suit');
    assert.equal(summary.elimination.length, 2, 'the two others go out before the fool is left');
});

test('the learner sees their own hand and nobody else\'s', async () => {
    const { frames } = await load();
    // The tutorial sits in seat 0: the kernel masks its boards exactly as it
    // would for a real player there. If this ever showed the whole table the
    // tutorial would be teaching from a cheat.
    for (const f of frames) {
        assert.equal(f.game.mySeat, LEARNER, 'the learner has a seat');
        assert.equal(f.game.seats[LEARNER].id, '', 'the learner\'s seat is nobody\'s account: the board says whose hand it is');
        assert.equal(f.game.myHand.length, f.game.seats[LEARNER].handCount,
            'the learner holds their real hand');
        for (const c of f.game.myHand) {
            assert.ok(c.suit >= 0 && c.value >= 0, 'the learner\'s own cards are face-up');
        }
    }
});

test('the tutorial teaches every element it narrates', async () => {
    const { summary, frames } = await load();
    const kinds = (k: number) => frames.filter((f) => f.kind === k);
    const learner = (k: number) => frames.filter((f) => f.kind === k && f.seat === LEARNER);
    const ps = summary.powerSuit;

    // The learner performs each move the tutorial prompts for...
    assert.ok(learner(REPLAY_STEP.ATTACK).length > 0, 'the learner attacks');
    assert.ok(learner(REPLAY_STEP.COVER).length > 0, 'the learner covers');
    assert.ok(learner(REPLAY_STEP.PASS).length > 0, 'the learner passes (perevod)');
    assert.ok(learner(REPLAY_STEP.PICKUP).length > 0, 'the learner picks up');

    // ...including a throw-in (an attack onto a table that is not empty)...
    const threwIn = frames.some((f, i) =>
        f.kind === REPLAY_STEP.ATTACK && f.seat === LEARNER
        && i > 0 && frames[i - 1].game.battles.length > 0);
    assert.ok(threwIn, 'the learner throws in');

    // ...and a trump cover, the one the beat calls out by name.
    const trumpCovered = learner(REPLAY_STEP.COVER).some((f) =>
        f.cards[0]?.suit === ps && f.target && f.target.suit !== ps);
    assert.ok(trumpCovered, 'the learner covers a plain card with a trump');

    // The learner says good. It never appears as a GOOD step — every good that
    // closes a bout becomes a seat-less ROUND_END — which is exactly why the
    // tutorial reads it off the board instead. If this assertion ever needed
    // GOOD steps, the learnerOwesGood path would be dead code.
    assert.equal(learner(REPLAY_STEP.GOOD).length, 0,
        'a finished game has no pending goods — the learner\'s goods close bouts');
    const owes = frames.filter((f, i) =>
        f.kind === REPLAY_STEP.ROUND_END && learnerOwesGood(frames[i - 1]));
    assert.ok(owes.length > 0, 'the learner is asked to say good at least once');

    // And the table shows the rest.
    assert.ok(kinds(REPLAY_STEP.ROUND_END).length > 0, 'a bout closes and the table is binned');
    assert.ok(frames.some((f) => f.seq.events.some((e) => e.type === 'refill')), 'players draw');
    assert.ok(frames.some((f) => f.seq.events.some((e) => e.type === 'out')), 'a player goes out');
    assert.ok(frames.some((f) => f.game.deckCount === 0 && !f.game.hasFlipped),
        'the stock runs out');
});

test('walking the tutorial the way a learner does reaches the end', async () => {
    const { frames } = await load();
    // The component auto-advances every step except the learner's own, where it
    // waits for the right button. Drive exactly that: if a learner step is ever
    // one the tutorial cannot prompt for, this stalls — which is what a beginner
    // would experience as a dead board.
    let i = 0;
    let prompts = 0;
    for (let guard = 0; i < frames.length - 1 && guard < 1000; guard++) {
        const next = i + 1;
        if (isLearnerStep(frames, next)) {
            const f = frames[next];
            const kind = f.kind === REPLAY_STEP.ROUND_END ? REPLAY_STEP.GOOD : f.kind;
            // Every learner step maps to a button the tutorial can highlight.
            const askable: readonly number[] = [REPLAY_STEP.ATTACK, REPLAY_STEP.PASS, REPLAY_STEP.PICKUP,
                       REPLAY_STEP.GOOD, REPLAY_STEP.COVER];
            assert.ok(askable.includes(kind),
                `step ${next} (kind ${f.kind}) is a move the learner can be asked for`);
            if (kind === REPLAY_STEP.ATTACK || kind === REPLAY_STEP.PASS || kind === REPLAY_STEP.COVER) {
                assert.ok(f.cards.length > 0, `step ${next} highlights the cards to play`);
            }
            if (kind === REPLAY_STEP.COVER) assert.ok(f.target, `step ${next} names the card to cover`);
            prompts++;
        }
        i = next;
    }
    assert.equal(i, frames.length - 1, 'the walkthrough reaches the last step');
    assert.ok(prompts >= 5, `the learner is asked to act ${prompts} times`);
});

test('the tutorial narrates the trump cover, and every concept exactly once', async () => {
    // THE GAME CONTAINING A TRUMP COVER IS NOT THE SAME CLAIM AS THE TUTORIAL
    // SAYING SO. The test above looks for one with `cards[0]` against `target`,
    // which is the very read buildBeats warns against: a step is one ACTION and
    // a multi-cover takes several attacks in it, so cards[0] and target are not
    // always a pair. Read that way the two can even come from different pairs.
    // So a learner could be shown a trump cover with no beat explaining it, or
    // the beat could fire on a step that has no such pair, and nothing here
    // would have noticed. This asks the builder itself.
    const { summary, frames } = await load();
    const beats = buildBeats(frames, summary, [...TUTORIAL_NAMES]);
    const ps = summary.powerSuit;

    // Head or rider: a collapsed beat teaches its extras in the same sentence,
    // so what matters is that the learner is TOLD on that step, not which slot
    // the key sits in.
    const teaches = (b: typeof beats[number], k: string) => b.key === k || (b.extras ?? []).includes(k as never);
    const trump = beats.find((b) => teaches(b, 'tut_trump_cover'));
    assert.ok(trump, `the tutorial teaches the trump cover (beats: ${beats.map((b) => [b.key, ...(b.extras ?? [])].join('+')).join(', ')})`);
    const f = frames[trump!.at];
    assert.equal(f.kind, REPLAY_STEP.COVER, 'the trump-cover beat sits on a cover');
    assert.ok((f.pairs ?? []).some((pr) => pr.card.suit === ps && pr.target.suit !== ps),
        'the step the beat names really spends a trump on a plain attack');

    // AT MOST ONE BEAT SHOWS AT A TIME - the component picks the latest at or
    // before the cursor - so a concept taught twice is a beat the learner never
    // sees, and two beats on one step is the same bug wearing a different hat.
    const keys = beats.flatMap((b) => [b.key, ...(b.extras ?? [])]);
    assert.equal(new Set(keys).size, keys.length, `each concept is taught once: ${keys.join(', ')}`);
    const ats = beats.map((b) => b.at);
    assert.deepEqual(ats, [...ats].sort((a, b) => a - b), 'beats are in step order');
    assert.equal(new Set(ats).size, ats.length, 'no two beats land on one step');

    // The moves the tutorial prompts for are the ones it explains.
    for (const k of ['tut_cover', 'tut_pass', 'tut_pickup', 'tut_throw_in', 'tut_draw'] as const) {
        assert.ok(keys.includes(k), `the tutorial teaches ${k}`);
    }
});
}

test('the trump-cover beat reads the PAIR, not the first card against the first target', () => {
    // A SYNTHETIC STEP, because the frozen tutorial cannot ask this question.
    // Its trump cover is a single pair, so `cards[0]` and `target` happen to BE
    // that pair and the wrong read passes - confirmed by mutating the builder
    // back to it and watching the fixture stay green. The read only matters on
    // a step that covers two attacks at once, which is a shape the tutorial
    // does not contain and a real game produces constantly.
    //
    // Here the trump is spent on the SECOND pair: cards[0] is a plain 9 on a
    // plain 7, and the trump 6 covers the 5. Reading cards[0] against target
    // sees no trump and teaches nothing; worse, it is comparing one pair's card
    // with another pair's attack, which can also invent a trump cover that was
    // never played.
    const S = { C: 0, D: 1, H: 2, S: 3 };
    const ps = S.S;
    const card = (value: number, suit: number) => ({ value, suit });
    const step = {
        kind: REPLAY_STEP.COVER,
        seat: LEARNER,
        cards: [card(9, S.H), card(6, ps)],
        target: card(7, S.H),
        pairs: [
            { card: card(9, S.H), target: card(7, S.H) },
            { card: card(6, ps), target: card(5, S.D) },
        ],
        seq: { events: [] },
        game: { battles: [], deckCount: 12, hasFlipped: false, seats: [] },
    };
    const deal = { ...step, kind: REPLAY_STEP.DEAL, cards: [], target: null, pairs: [] };
    const frames = [deal, step] as unknown as ReplayFrame[];
    const summary = { powerSuit: ps, firstAttacker: LEARNER, fool: 1 } as never;

    const beats = buildBeats(frames, summary, ['You', 'Ada', 'Bo']);
    const keys = beats.flatMap((b) => [b.key, ...(b.extras ?? [])]);
    assert.ok(keys.includes('tut_trump_cover'),
        `a trump on the second pair is still a trump cover (beats: ${keys.join(', ')})`);
});

if (!process.env.VALIDATION_ONLY) registerTutorialValidation();
