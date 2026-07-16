/* =============================================================================
 * A5 — the web's replay is the game the engine played
 * =============================================================================
 * e2e/replay_steps_frames.test.ts proves the kernel's frames decode with the
 * live decoder. This is the layer above: src/replay/frames.ts, the thing the
 * replay screen actually renders, driven exactly as the browser drives it.
 *
 * What it has to establish, because a replay screen is otherwise very good at
 * looking right while being wrong:
 *
 *   1. every board is the board the engine really played (not a re-derivation);
 *   2. the reveal-hands eye shows what each seat REALLY held — the old screen
 *      retrodicted this and could be confidently wrong;
 *   3. the status line's kinds come from the kernel and match the real game,
 *      passes included;
 *   4. cards are conserved at every step, which is what a desync looks like.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import { game_done } from '../supabase/functions/_shared/common_utils.ts';
import {
    Card, Game, GAME_STATUS, PLAYER_STATUS, PrivatePlayer, StrategyKey,
} from '../supabase/functions/_shared/types.ts';
import { shouldBotActCore, processBotAction } from '../supabase/functions/_shared/pure_bot_actions.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/bot_strategy.ts';
import { kernelReplayEncodeV6FromGame } from '../supabase/functions/_shared/wasm/bots.ts';
import { __setDealSeedOverride } from '../supabase/functions/_shared/wasm/engine.ts';
import { deckSizeFor } from '../supabase/functions/_shared/constants.ts';
import {
    buildReplayFrames, buildReverseFrames, preDealGame, stepTimes, REPLAY_STEP,
} from '../src/replay/frames.ts';

if (!process.env.E2E_VERBOSE) {
    console.log = () => {};
    console.warn = () => {};
    console.error = () => {};
    console.info = () => {};
}

const hexToBytes = (h: string) =>
    new Uint8Array((h.match(/.{2}/g) ?? []).map(b => parseInt(b, 16)));

const seedFor = (np: number, s: number) =>
    hexToBytes(Array.from({ length: 32 }, (_, i) =>
        (((i * 31 + s * 13 + np) & 0xff)).toString(16).padStart(2, '0')).join(''));

const mkPlayer = (i: number, strategy: StrategyKey): PrivatePlayer => ({
    player_id: `bot_${i}`, name: `Bot ${i}`, status: PLAYER_STATUS.READY,
    is_ai: true, hand: [], awaiting_attack: false, hand_length: 0,
    strategy_key: strategy,
});

function mkGame(np: number, seedHex: string): Game {
    return {
        players: Array.from({ length: np }, (_, i) => mkPlayer(i, 'handwritten' as StrategyKey)),
        deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.PLAYING,
        deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
        first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
        good_timestamp: null, good_players: [], game_seed: seedHex,
    } as unknown as Game;
}

/** A seeded game played to the end, exactly as the finalize path sees one. */
async function playSeeded(np: number, s: number): Promise<{ game: Game; code: Uint8Array } | null> {
    const seed = seedFor(np, s);
    const seedHex = Array.from(seed).map(b => b.toString(16).padStart(2, '0')).join('');
    const game = mkGame(np, seedHex);
    __setDealSeedOverride(seed);
    try {
        start_game(game);
        for (let guard = 0; guard < 20000 && game_done(game) === null; guard++) {
            let acted = false;
            for (let i = 0; i < game.players.length && !acted; i++) {
                const p = game.players[i];
                if (!shouldBotActCore(game, p, i)) continue;
                if (calculateLegalMoves(game, p.player_id).length === 0) continue;
                acted = await processBotAction(game, p);
            }
            if (!acted) return null;
        }
    } finally {
        __setDealSeedOverride(null);
    }
    if (game_done(game) === null) return null;
    return { game, code: kernelReplayEncodeV6FromGame(game, seed, undefined, 1 << 20) };
}

const key = (c: Card) => `${c.suit}-${c.value}`;

test('every step of a web replay is a board the engine really played', async () => {
    for (let np = 2; np <= 4; np++) {
        const played = await playSeeded(np, 500 + np);
        assert.ok(played, `${np}p seeded game completed`);
        if (!played) continue;
        const { game, code } = played;

        const frames = buildReplayFrames(code, 'g', null, null);
        assert.ok(frames.length > 1, `${np}p: the code has steps`);

        // The closing board is the board the engine finished on. Not "a board
        // consistent with" it — the same one.
        const last = frames[frames.length - 1].game;
        assert.equal(last.discard_pile_length, game.discard_pile_length,
            `${np}p: ends on the played discard count`);
        assert.equal(last.deck_length, 0, `${np}p: a finished game drained its stock`);
        assert.equal(last.players.length, np, `${np}p: every seat came back`);
        for (let s = 0; s < np; s++) {
            assert.equal(last.players[s].hand_length, game.players[s].hand.length,
                `${np}p: seat ${s} ends holding what it really held`);
        }

        // Cards are conserved at EVERY step: hands + table + stock + flip +
        // discard is the whole deck, always. A desynced replay fails here.
        const deckSize = deckSizeFor(np);
        frames.forEach((f, i) => {
            const inHands = f.game.players.reduce((sum, p) => sum + p.hand_length, 0);
            const onTable = f.game.table_battles.reduce(
                (sum, b) => sum + 1 + (b.defense ? 1 : 0), 0);
            const total = inHands + onTable + f.game.deck_length
                + (f.game.flipped ? 1 : 0) + f.game.discard_pile_length;
            assert.equal(total, deckSize, `${np}p step ${i}: ${total} cards accounted for`);
        });
    }
});

test('the reveal eye shows the hand a seat REALLY held, not a guess', async () => {
    // The old screen retrodicted this: it bound each revealed card back to the
    // oldest face-down slot that could have held it. That is a consistent guess
    // and nothing more. v6 is hidden-state-lossless, so this must be exact — at
    // the FINAL step, where the played game's own hands are there to check.
    for (let np = 2; np <= 4; np++) {
        const played = await playSeeded(np, 500 + np);
        assert.ok(played, `${np}p seeded game completed`);
        if (!played) continue;
        const { game, code } = played;

        const frames = buildReplayFrames(code, 'g', null, null);
        const hands = frames[frames.length - 1].game.replay_hands;
        assert.equal(hands.length, np, `${np}p: a hand per seat`);

        for (let s = 0; s < np; s++) {
            const shown = hands[s].map(c => c && key(c)).sort();
            const real = game.players[s].hand.map(c => key(c)).sort();
            assert.deepEqual(shown, real, `${np}p: seat ${s}'s revealed hand is its real hand`);
            assert.ok(!hands[s].includes(null), `${np}p: seat ${s} has no unknown cards`);
        }

        // ...and mid-game too: hand SIZES must track the board at every step,
        // which is what catches a per-seat replay drifting out of step order.
        frames.forEach((f, i) => {
            f.game.replay_hands.forEach((h, s) => {
                assert.equal(h.length, f.game.players[s].hand_length,
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
            const played = await playSeeded(np, 900 + s);
            if (!played) continue;
            const { game, code } = played;

            const count = (t: string) => game.logs.filter(l => l.log_type === t).length;
            if (count('pass') === 0) continue;
            sawPass = true;

            const frames = buildReplayFrames(code, 'g', null, null);
            const kinds = (k: number) => frames.filter(f => f.kind === k).length;
            assert.equal(kinds(REPLAY_STEP.PASS), count('pass'), `${np}p: passes are passes`);
            assert.equal(kinds(REPLAY_STEP.ATTACK), count('attack'), `${np}p: attacks are attacks`);
            assert.equal(kinds(REPLAY_STEP.COVER), count('cover'), `${np}p: covers are covers`);
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
    const played = await playSeeded(3, 503);
    assert.ok(played);
    if (!played) return;

    const frames = buildReplayFrames(played.code, 'g', null, null);
    const reverse = buildReverseFrames(frames);

    assert.equal(reverse.length, frames.length, 'a reverse per step');
    assert.equal(reverse[0], null, 'nothing precedes the deal');
    for (let i = 1; i < frames.length; i++) {
        const rev = reverse[i]!;
        // The board a step-back commits is the kernel's own previous board —
        // never a rewind computed from the animation.
        assert.equal(rev.game, frames[i - 1].game, `step ${i} back lands on step ${i - 1}'s board`);
        assert.equal(rev.events.length, 1, `step ${i} back is one flight`);
        assert.equal(rev.events[0].game_state, frames[i - 1].game,
            `step ${i} back's flight ends on that board`);
    }
});

test('the deal animates out of an empty table, and the clock tracks the moves', async () => {
    const played = await playSeeded(3, 504);
    assert.ok(played);
    if (!played) return;

    const frames = buildReplayFrames(played.code, 'g', null, null);

    const pre = preDealGame(frames[0]);
    assert.equal(pre.deck_length, deckSizeFor(3), 'the stock starts whole');
    assert.equal(pre.flipped, null, 'nothing is flipped yet');
    assert.deepEqual(pre.players.map(p => p.hand_length), [0, 0, 0], 'no one has been dealt to');
    assert.equal(pre.table_battles.length, 0, 'the table is empty');

    // One recorded gap per attack/cover/pass/pickup — the exact set of step
    // kinds the clock advances on. If those ever drift apart the timestamps slide
    // silently, so check the arithmetic end to end.
    const timed = frames.filter(f =>
        [REPLAY_STEP.ATTACK, REPLAY_STEP.COVER, REPLAY_STEP.PASS, REPLAY_STEP.PICKUP]
            .includes(f.kind)).length;
    const gaps = Array.from({ length: timed }, () => 10);
    const times = stepTimes(frames, 1000, gaps);
    assert.equal(times[0], 1000, 'the deal is the start time');
    assert.equal(times[times.length - 1], 1000 + 10 * timed, 'every gap is spent, exactly once');

    assert.deepEqual(stepTimes(frames, null, null), frames.map(() => null),
        'no timing data, no timestamps');
});
