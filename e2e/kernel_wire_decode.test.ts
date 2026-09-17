/* =============================================================================
 * The wire decode says what the engine actually did (A8/F7, A9)
 * =============================================================================
 * This file began as a twin test: the kernel's reader vs the TypeScript mirror
 * that shadowed view.c and evwire.c byte for byte. That mirror is deleted, so
 * there is no second implementation left to agree with - and agreeing with a
 * copy was always the weaker claim anyway. It could only ever say "the two
 * match", never "the answer is right" (docs/C_CORE_CONSOLIDATION.md A9).
 *
 * What it asserts now is the strong version: decode the frames and you get back
 * the game the ENGINE PLAYED. The oracle is a real seeded game, played by the
 * kernel's own bot cycle on a C Table and encoded by the finalize path's encoder
 * (helpers/replay_play.ts), and the closing board is the one that table serves -
 * not a frozen fixture, which in this repo only ever means a fixture that has
 * quietly rotted.
 *
 * Every frame is read the way the web reads it: the client slot's live push
 * reader (sdk/ts/table/client_table.ts), with no identity, as a replay is.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { ensureBotsAsync, replayEventFrames } from '../sdk/ts/wasm/bots.ts';
import { clientTable } from '../sdk/ts/table/client_table.ts';
import * as V from '../sdk/ts/gen/view_layout.bots.ts';
import { pushToSequence } from '../src/state/pushSequence.ts';
import { seedBytes } from './helpers/bot_table.ts';
import { playRecorded } from './helpers/replay_play.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

/** A replay frame through the live reader, as the web's animation pipeline receives it; null when it does not read whole. */
const readFrame = (bytes: Uint8Array) => {
    const read = clientTable().readPush(bytes, { as3: false, identity: 'none' });
    return read ? pushToSequence(read) : null;
};

const robusta = (np: number, s: number) => playRecorded(Array(np).fill('robusta'), seedBytes(np, s));

// The last frame of a finished game carries the committed final board as its
// trailer. That board must be the one the table ended on - the whole decode
// path (replay rebuild, frame writer, client reader) held against the real thing.
test('the decoded final board is the board the engine actually ended on', async () => {
    await ensureBotsAsync();
    for (const [np, seed] of [[2, 5], [3, 71], [4, 12]] as const) {
        const played = robusta(np, seed);
        const ended = played.spectatorView;

        const frames = replayEventFrames(played.code, -1);
        const last = readFrame(frames[frames.length - 1]);
        assert.ok(last, 'the last frame decodes');

        const board = last!.game;
        assert.equal(board.deckCount, ended.deckCount, `${np}p: the deck the engine left`);
        assert.equal(board.discardPileLength, ended.discardPileLength, `${np}p: the discard`);
        assert.equal(board.powerSuit, ended.powerSuit, `${np}p: the trump`);
        assert.equal(board.battles.length, ended.battles.length, `${np}p: the table`);
        assert.deepEqual(board.seats.map((p) => p.handCount),
                         ended.seats.map((p) => p.handCount),
                         `${np}p: every seat's hand size`);
        // The table parks every seat once it finalizes the win (bots READY), so
        // who went out, and in which order, is read off its elimination order.
        assert.deepEqual(board.elimination, ended.elimination,
                         `${np}p: who went out, in the order they did`);
        assert.equal(board.seats.filter((p) => p.status === V.PLAYER_STATUS_OUT).length,
                     np - 1, `${np}p: everyone but the fool is out`);
        assert.equal(board.seats.findIndex((p) => p.status !== V.PLAYER_STATUS_OUT), played.fool,
                     `${np}p: the seat left in is the table's fool`);
    }
});

// Masking is per-viewer and the kernel does it (view.c), but a decoder that
// dropped it on the floor would still produce a plausible-looking board. A
// spectator must see no card identities at all; the seat itself must see its own.
test('a spectator decode carries no hand, and a seat decode carries its own', async () => {
    await ensureBotsAsync();
    const code = robusta(3, 71).code;

    // The deal frame - the one place every hand is full and a leak would show.
    const spectator = readFrame(replayEventFrames(code, -1)[0]);
    assert.ok(spectator, 'the deal frame decodes for a spectator');
    assert.equal(spectator!.game.mySeat, -1, 'a spectator sits nowhere');
    assert.deepEqual(spectator!.game.myHand, [], 'a spectator holds no hand');

    // DEAL and REFILL are the redacted types: a card bound for someone's hand.
    // The FLIPPED trump is public by the rules of the game and must NOT be a
    // back - asserting "every card is a back" would have been wrong, and was:
    // measured, the deal frame is 3 fully-masked DEALs plus one real trump.
    let dealt = 0, flipped = 0;
    for (const ev of spectator!.events) {
        const cards = ev.cards ?? [];
        if (ev.type === 'deal' || ev.type === 'refill') {
            for (const c of cards) {
                assert.ok(c.suit < 0 && c.value < 0,
                          `a card dealt to a hand is a back for a spectator (got ${c.suit}/${c.value})`);
            }
            dealt += cards.length;
        }
        if (ev.type === 'flipped') {
            for (const c of cards) assert.ok(c.suit >= 0, 'the trump is public');
            flipped += cards.length;
        }
    }
    assert.ok(dealt > 0, `${dealt} dealt cards were checked - the loop is not vacuous`);
    assert.equal(flipped, 1, 'and exactly one trump was turned');

    const seat1 = readFrame(replayEventFrames(code, 1)[0]);
    assert.ok(seat1, 'the deal frame decodes for seat 1');
    assert.equal(seat1!.game.mySeat, 1, 'seat 1 sits in seat 1');
    assert.ok(seat1!.game.myHand.length > 0, 'holding cards');
    assert.ok(seat1!.game.myHand.every((c) => c.suit >= 0), "and seat 1's own cards are face-up");

    // The seat sees its OWN dealt cards, and no one else's: the DEAL events
    // aimed at other seats stay backs even in seat 1's own stream.
    const ownDeal = seat1!.events.some((e) => (e.cards ?? []).some((c) => c.suit >= 0) && e.type === 'deal');
    assert.ok(ownDeal, 'seat 1 watches its own cards arrive');
    const othersDeal = seat1!.events.filter((e) => e.type === 'deal' && e.seat !== undefined && e.seat !== 1);
    assert.ok(othersDeal.length > 0, 'the other seats are dealt in the same stream');
    for (const e of othersDeal) {
        assert.ok((e.cards ?? []).every((c) => c.suit < 0), `seat ${e.seat}'s dealt cards are backs to seat 1`);
    }
});

// An unreadable payload must refuse. Half a sequence rendered as a whole one is
// worse than no sequence: the board would silently be wrong.
test('an unreadable payload decodes to null rather than a wrong board', async () => {
    await ensureBotsAsync();
    const frame = replayEventFrames(robusta(3, 71).code, -1)[0];

    assert.ok(readFrame(frame), 'the whole frame reads (the control)');

    for (const len of [1, 4, 8, Math.floor(frame.length / 2), frame.length - 1]) {
        assert.equal(readFrame(frame.subarray(0, len)), null,
                     `a ${len}-byte prefix is unreadable, not a partial board`);
    }

    const foreign = frame.slice();
    foreign[0] = 99;
    assert.equal(readFrame(foreign), null,
                 'a format this build does not read is refused, not guessed at');

    // And the kernel says why, rather than returning an empty sequence.
    assert.equal(clientTable().lastRefusal().code, V.CLIENT_E_PUSH, 'refused as a push that does not read');
});
