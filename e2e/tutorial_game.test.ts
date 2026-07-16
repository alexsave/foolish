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
import assert from 'node:assert/strict';

import { codeToGame, bigintToBytes } from '../supabase/functions/_shared/common/replay/codec.ts';
import { decodeReplay } from '../supabase/functions/_shared/common/replay/decode.ts';
import { buildReplayFrames, REPLAY_STEP, ReplayFrame } from '../src/replay/frames.ts';
import { TUTORIAL_MOVES_CODE, TUTORIAL_NAMES } from '../src/components/tutorialGame.ts';
import { PLAYER_STATUS } from '../supabase/functions/_shared/core/types.ts';

if (!process.env.E2E_VERBOSE) {
    console.log = () => {};
    console.warn = () => {};
    console.error = () => {};
}

const LEARNER = 0;
const SELF_ID = 'seat-0';

// Mirrors Tutorial.tsx's learnerOwesGood — a good that CLOSES a bout is not
// attributed to anyone (v6 records the round ending, not who ended it), so the
// learner's own closing good arrives as a seat-less ROUND_END.
const learnerOwesGood = (prev: ReplayFrame | undefined): boolean => {
    if (!prev) return false;
    const me = prev.game.players[LEARNER];
    return !!me && me.status !== PLAYER_STATUS.OUT
        && prev.game.defender !== LEARNER
        && !prev.game.good_players.includes(SELF_ID);
};

const isLearnerStep = (frames: ReplayFrame[], i: number): boolean => {
    if (i < 0 || i >= frames.length) return false;
    const f = frames[i];
    if (f.kind === REPLAY_STEP.ROUND_END) return learnerOwesGood(frames[i - 1]);
    return f.seat === LEARNER && [
        REPLAY_STEP.ATTACK, REPLAY_STEP.COVER, REPLAY_STEP.PASS,
        REPLAY_STEP.PICKUP, REPLAY_STEP.GOOD,
    ].includes(f.kind);
};

const load = async () => {
    const x = codeToGame(TUTORIAL_MOVES_CODE);
    const decoded = await decodeReplay(x);
    const frames = buildReplayFrames(bigintToBytes(x), 'tutorial', TUTORIAL_NAMES, {
        viewer: LEARNER, fool: decoded.fool,
    });
    return { decoded, frames };
};

export function registerTutorialValidation(): void {
test('the tutorial code still replays on the kernel that ships', async () => {
    const { decoded, frames } = await load();
    assert.equal(decoded.formatVersion, 6, 'the tutorial is a v6 code (v5 cannot replay)');
    assert.equal(decoded.playerCount, 3, '3-player game');
    assert.ok(frames.length > 10, `replays to ${frames.length} steps`);
    assert.equal(frames[0].kind, REPLAY_STEP.DEAL, 'it opens with the deal');
    assert.notEqual(decoded.fool, LEARNER, 'the learner is not left the fool');
    assert.equal(decoded.firstAttacker, LEARNER, 'the learner holds the lowest trump and leads');
});

test('the learner sees their own hand and nobody else\'s', async () => {
    const { frames } = await load();
    // The tutorial sits in seat 0: the kernel masks its boards exactly as it
    // would for a real player there. If this ever showed the whole table the
    // tutorial would be teaching from a cheat.
    for (const f of frames) {
        assert.ok(f.game.self, 'the learner has a self');
        assert.equal(f.game.self.hand.length, f.game.players[LEARNER].hand_length,
            'the learner holds their real hand');
        for (const c of f.game.self.hand) {
            assert.ok(c.suit >= 0 && c.value >= 0, 'the learner\'s own cards are face-up');
        }
    }
});

test('the tutorial teaches every element it narrates', async () => {
    const { decoded, frames } = await load();
    const kinds = (k: number) => frames.filter((f) => f.kind === k);
    const learner = (k: number) => frames.filter((f) => f.kind === k && f.seat === LEARNER);
    const ps = decoded.powerSuit;

    // The learner performs each move the tutorial prompts for...
    assert.ok(learner(REPLAY_STEP.ATTACK).length > 0, 'the learner attacks');
    assert.ok(learner(REPLAY_STEP.COVER).length > 0, 'the learner covers');
    assert.ok(learner(REPLAY_STEP.PASS).length > 0, 'the learner passes (perevod)');
    assert.ok(learner(REPLAY_STEP.PICKUP).length > 0, 'the learner picks up');

    // ...including a throw-in (an attack onto a table that is not empty)...
    const threwIn = frames.some((f, i) =>
        f.kind === REPLAY_STEP.ATTACK && f.seat === LEARNER
        && i > 0 && frames[i - 1].game.table_battles.length > 0);
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
    assert.ok(frames.some((f) => f.game.deck_length === 0 && f.game.flipped === null),
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
            assert.ok([REPLAY_STEP.ATTACK, REPLAY_STEP.PASS, REPLAY_STEP.PICKUP,
                       REPLAY_STEP.GOOD, REPLAY_STEP.COVER].includes(kind),
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
}

if (!process.env.VALIDATION_ONLY) registerTutorialValidation();
