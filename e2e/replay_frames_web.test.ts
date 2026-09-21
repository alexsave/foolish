/* =============================================================================
 * A5 - the web's replay is the game the engine played
 * =============================================================================
 * e2e/replay_steps_frames.test.ts proves the kernel's frames decode with the
 * live decoder. This is the layer above: src/replay/frames.ts, the thing the
 * replay screen actually renders, driven exactly as the browser drives it.
 *
 * What it has to establish, because a replay screen is otherwise very good at
 * looking right while being wrong:
 *
 *   1. every board is the board the engine really played (not a re-derivation);
 *   2. the reveal-hands eye shows what each seat REALLY held - the old screen
 *      retrodicted this and could be confidently wrong;
 *   3. the status line's kinds come from the kernel and match the real game,
 *      passes included;
 *   4. cards are conserved at every step, which is what a desync looks like;
 *   5. the board before the deal is the kernel's (client_board_edit UNDEAL): the
 *      whole stock, and no card in anyone's hand, the watching seat's included;
 *   6. a replay's seats are named by the code's extras, and carry no invented
 *      player id (Phase 7: the boards are snapshots of the replay's own frames).
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { covered } from '../src/state/view.ts';
import {
    buildReplayFrames, buildReverseFrames, preDealGame, stepTimes, REPLAY_STEP,
} from '../src/replay/frames.ts';
import { seedBytes } from './helpers/bot_table.ts';
import { cardKey as key, movesOf, playRecorded, type RecordedGame } from './helpers/replay_play.ts';

if (!process.env.E2E_VERBOSE) {
    console.log = () => {};
    console.warn = () => {};
    console.error = () => {};
    console.info = () => {};
}

/**
 * A seeded game played to the end by the kernel's bot cycle on a C Table, with
 * the boards its table served and the pushes it sent: the truth each replay
 * below is held against (helpers/replay_play.ts).
 */
const playSeeded = (np: number, s: number): RecordedGame => playRecorded(Array(np).fill('handwritten'), seedBytes(np, s));

test('every step of a web replay is a board the engine really played', async () => {
    for (let np = 2; np <= 4; np++) {
        const { spectatorView: ended, code } = playSeeded(np, 500 + np);

        const frames = buildReplayFrames(code, 'g', null);
        assert.ok(frames.length > 1, `${np}p: the code has steps`);

        // The closing board is the board the engine finished on. Not "a board
        // consistent with" it - the same one.
        const last = frames[frames.length - 1].game;
        assert.equal(last.discardPileLength, ended.discardPileLength,
            `${np}p: ends on the played discard count`);
        assert.equal(last.deckCount, 0, `${np}p: a finished game drained its stock`);
        assert.equal(last.seats.length, np, `${np}p: every seat came back`);
        for (let s = 0; s < np; s++) {
            assert.equal(last.seats[s].handCount, ended.seats[s].handCount,
                `${np}p: seat ${s} ends holding what it really held`);
        }

        // Cards are conserved at EVERY step: hands + table + stock + flip +
        // discard is the whole deck, always. A desynced replay fails here. The
        // whole deck is the stock the deal was dealt from.
        const deckSize = preDealGame(frames[0]).deckCount;
        assert.equal(deckSize, np >= 6 ? 52 : 36, `${np}p: the deck the seat count plays with`);
        frames.forEach((f, i) => {
            const inHands = f.game.seats.reduce((sum, p) => sum + p.handCount, 0);
            const onTable = f.game.battles.reduce(
                (sum, b) => sum + 1 + (covered(b) ? 1 : 0), 0);
            const total = inHands + onTable + f.game.deckCount
                + (f.game.hasFlipped ? 1 : 0) + f.game.discardPileLength;
            assert.equal(total, deckSize, `${np}p step ${i}: ${total} cards accounted for`);
        });
    }
});

test('the reveal eye shows the hand a seat REALLY held, not a guess', async () => {
    // The old screen retrodicted this: it bound each revealed card back to the
    // oldest face-down slot that could have held it. That is a consistent guess
    // and nothing more. v6 is hidden-state-lossless, so this must be exact - at
    // the FINAL step, where the played game's own hands are there to check.
    for (let np = 2; np <= 4; np++) {
        const { seatViews, code } = playSeeded(np, 500 + np);

        const frames = buildReplayFrames(code, 'g', null);
        const hands = frames[frames.length - 1].game.replay_hands;
        assert.equal(hands.length, np, `${np}p: a hand per seat`);

        for (let s = 0; s < np; s++) {
            const shown = hands[s].map(c => c && key(c)).sort();
            // The hand the table served seat s at the end: its own cards, face up.
            const real = seatViews[s].myHand.map(c => key(c)).sort();
            assert.deepEqual(shown, real, `${np}p: seat ${s}'s revealed hand is its real hand`);
            assert.ok(!hands[s].includes(null), `${np}p: seat ${s} has no unknown cards`);
        }

        // ...and mid-game too: hand SIZES must track the board at every step,
        // which is what catches a per-seat replay drifting out of step order.
        frames.forEach((f, i) => {
            f.game.replay_hands.forEach((h, s) => {
                assert.equal(h.length, f.game.seats[s].handCount,
                    `${np}p step ${i}: seat ${s}'s revealed hand matches its count`);
            });
        });
    }
});

test('the status line asks the kernel what happened, and gets the real game back', async () => {
    // A pass and an attack are ONE event type on the wire. If the screen ever
    // goes back to inferring the difference from prose, this catches it.
    let sawPass = false;
    for (let np = 3; np <= 4; np++) {
        for (let s = 0; s < 12 && !sawPass; s++) {
            const { events, code } = playSeeded(np, 900 + s);

            // What live play pushed: one attack_pass event per attack or pass,
            // told apart by the kernel's message code, not by prose.
            const moves = movesOf(events);
            const count = (t: keyof typeof moves) => moves[t];
            if (count('pass') === 0) continue;
            sawPass = true;

            const frames = buildReplayFrames(code, 'g', null);
            const kinds = (k: number) => frames.filter(f => f.kind === k).length;
            assert.equal(kinds(REPLAY_STEP.PASS), count('pass'), `${np}p: passes are passes`);
            assert.equal(kinds(REPLAY_STEP.ATTACK), count('attack'), `${np}p: attacks are attacks`);
            // A COVER FRAME IS A MOVE, NOT A PAIR. Live play pushes one cover
            // event per pair, and the replay wire codes one step per pair, but
            // buildReplayFrames merges a defender's consecutive covers back
            // into the single move they were. So the pairs must still all be
            // there - none invented, none dropped - while the frames that carry
            // them are fewer.
            const coverFrames = frames.filter((f) => f.kind === REPLAY_STEP.COVER);
            const pairs = coverFrames.reduce((n, f) => n + (f.pairs?.length ?? 0), 0);
            assert.equal(pairs, count('cover'), `${np}p: every cover pair survives merging`);
            assert.equal(coverFrames.reduce((n, f) => n + f.moves, 0), count('cover'),
                `${np}p: a merged frame counts the moves it merged`);
            assert.ok(coverFrames.length <= count('cover'), `${np}p: merging never adds frames`);
            // Each frame's own cards and events agree with its pair count, so a
            // merge cannot quietly lose the second card of a double cover.
            for (const f of coverFrames) {
                assert.equal(f.cards.length, f.pairs?.length ?? 0, `${np}p: a cover frame's cards are its pairs`);
                assert.equal(f.seq.events.filter((e: { type: string }) => e.type === 'cover').length,
                    f.pairs?.length ?? 0, `${np}p: a cover frame animates every pair`);
            }
            assert.equal(kinds(REPLAY_STEP.PICKUP), count('pickup'), `${np}p: pickups are pickups`);

            assert.equal(frames[0].kind, REPLAY_STEP.DEAL, 'step 0 is the deal');
            assert.equal(frames[0].seat, null, 'the deal is nobody\'s action');
            // Every acting step names its seat, or the status line has nobody
            // to credit the move to.
            for (const f of frames.slice(1)) {
                if (f.kind === REPLAY_STEP.ROUND_END) continue;
                assert.ok(f.seat !== null && f.seat >= 0 && f.seat < np,
                    `${np}p: a ${f.kind} step names a real seat`);
            }
            // The cards the status line shows are the cards that moved.
            for (const f of frames) {
                if (f.kind === REPLAY_STEP.ATTACK || f.kind === REPLAY_STEP.PASS
                    || f.kind === REPLAY_STEP.COVER) {
                    assert.ok(f.cards.length > 0, `a ${f.kind} step shows the cards it played`);
                }
                if (f.kind === REPLAY_STEP.COVER) {
                    assert.ok(f.target, 'a cover shows what it covered');
                }
            }
        }
    }
    assert.ok(sawPass, 'found a seeded game containing a pass');
});

test('stepping back lands on the previous step\'s real board', async () => {
    const played = playSeeded(3, 503);

    const frames = buildReplayFrames(played.code, 'g', null);
    const reverse = buildReverseFrames(frames);

    assert.equal(reverse.length, frames.length, 'a reverse per step');
    assert.equal(reverse[0], null, 'nothing precedes the deal');
    for (let i = 1; i < frames.length; i++) {
        const rev = reverse[i]!;
        // The board a step-back commits is the kernel's own previous board -
        // never a rewind computed from the animation.
        assert.equal(rev.game, frames[i - 1].game, `step ${i} back lands on step ${i - 1}'s board`);
        assert.equal(rev.events.length, 1, `step ${i} back is one flight`);
        assert.equal(rev.events[0].game_state, frames[i - 1].game,
            `step ${i} back's flight ends on that board`);
    }
});

test('the deal animates out of an empty table, and the clock tracks the moves', async () => {
    const played = playSeeded(3, 504);

    const frames = buildReplayFrames(played.code, 'g', null);

    const pre = preDealGame(frames[0]);
    const dealt = frames[0].game;
    assert.equal(pre.deckCount, dealt.deckCount + (dealt.hasFlipped ? 1 : 0) + dealt.seats.reduce((n, p) => n + p.handCount, 0),
        'the stock starts whole: every card the deal handed out is still in it');
    assert.equal(pre.hasFlipped, false, 'nothing is flipped yet');
    assert.deepEqual(pre.seats.map(p => p.handCount), [0, 0, 0], 'no one has been dealt to');
    assert.equal(pre.battles.length, 0, 'the table is empty');

    // One recorded gap per attack/cover/pass/pickup - the exact set of step
    // kinds the clock advances on. If those ever drift apart the timestamps slide
    // silently, so check the arithmetic end to end.
    const timed = frames.filter(f =>
        ([REPLAY_STEP.ATTACK, REPLAY_STEP.COVER, REPLAY_STEP.PASS, REPLAY_STEP.PICKUP] as number[])
            .includes(f.kind)).length;
    const gaps = Array.from({ length: timed }, () => 10);
    const times = stepTimes(frames, 1000, gaps);
    assert.equal(times[0], 1000, 'the deal is the start time');
    assert.equal(times[times.length - 1], 1000 + 10 * timed, 'every gap is spent, exactly once');

    assert.deepEqual(stepTimes(frames, null, null), frames.map(() => null),
        'no timing data, no timestamps');
});

test('before the deal lands, nobody holds a card - the watching seat included', async () => {
    // A replay watched from a seat (the tutorial's learner) is masked for that
    // seat, so its first frame already shows that seat's dealt hand. The board
    // the deal animates onto must not: the cards have not been dealt yet.
    const played = playSeeded(3, 504);
    for (const viewer of [-1, 0, 2]) {
        const frames = buildReplayFrames(played.code, 'g', null, { viewer });
        assert.equal(frames[0].game.mySeat, viewer, `the frames are watched from ${viewer}`);
        const pre = preDealGame(frames[0]);
        assert.deepEqual(pre.myHand, [], `viewer ${viewer}: my hand is empty before the deal`);
        assert.deepEqual(pre.seats.map((p) => p.handCount), [0, 0, 0], `viewer ${viewer}: every hand is empty`);
        assert.equal(pre.mySeat, viewer, `viewer ${viewer}: still watched from the same seat`);
        assert.deepEqual(pre.replay_hands, [[], [], []], `viewer ${viewer}: the reveal eye shows no hand either`);
    }
});

test('a replay\'s seats are named by its extras, and carry no invented player id', async () => {
    const played = playSeeded(3, 505);
    const named = buildReplayFrames(played.code, 'g', ['Ada', null, 'Cy']);
    const plain = buildReplayFrames(played.code, 'g', null, { fool: 1 });
    for (const [frames, names] of [[named, ['Ada', 'P2', 'Cy']], [plain, ['P1', 'P2', 'P3']]] as const) {
        for (const f of [frames[0], frames[frames.length >> 1]]) {
            assert.deepEqual(f.game.seats.map((s) => s.id), ['', '', ''], 'no seat has a player id');
            assert.deepEqual(f.game.seats.map((s) => s.name), names, 'the seats are named by the extras, or P1, P2...');
            assert.equal(f.game.gameId, 'g', 'the board is the replay\'s');
            for (const e of f.seq.events) {
                const board = (e as unknown as { game_state?: { seats: readonly { id: string }[] } }).game_state;
                if (board) assert.deepEqual(board.seats.map((s) => s.id), ['', '', ''], 'nor on an event\'s board');
            }
        }
    }
});
