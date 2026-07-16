/* =============================================================================
 * The kernel reads the wire the same way the TS mirror did (A8/F7)
 * =============================================================================
 * The web used to decode view.c's and evwire.c's formats with hand-written
 * TypeScript that shadowed the C byte for byte. This is the cutover harness for
 * replacing that with the kernel's own reader: on real frames, from a real
 * seeded game, the two must agree on every field the packed bytes actually
 * carry.
 *
 * What they cannot agree on, and why that is not a gap:
 *   * identity — player_id/name/is_ai. game.h is explicit that seat identity is
 *     not in the state blob, so the kernel emits ""/0 and the roster join stays
 *     host-side (viewToGame). Compared here only where the blob has the answer.
 *   * good_players ORDER — the kernel emits the mask; the insertion order needs
 *     the caller's prior order (goodPlayersFromViewMask), which is host memory.
 *   * good_timestamp VALUE — a host clock reading the kernel never took. The
 *     kernel carries whether one exists (hasGoodTs); the value is the host's.
 *   * message prose — the kernel emits the template code; the sentence is
 *     reconstructed host-side from the roster.
 *
 * Once the mirror is deleted this file stops being a twin test — there is no
 * second implementation left to agree with — and what survives is the part that
 * was always the stronger claim: the decode reproduces the board the ENGINE
 * played (A9).
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    ensureBotsAsync, kernelViewFromPacked, kernelEventsFromPacked,
    replayEventFrames, KernelState,
} from '../supabase/functions/_shared/wasm/bots.ts';
import { parseMaskedState, ViewState } from '../supabase/functions/_shared/wire/view.ts';
import { playSeededV6 } from './helpers/seeded_game.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

// The two decoders describe the same board in different vocabularies (the
// mirror's ViewState vs the kernel's KernelState). Reduce both to the fields the
// blob genuinely carries, so the comparison is about the DECODE and not about
// which shape each side happens to like.
const fromMirror = (v: ViewState) => ({
    status: v.status,
    numPlayers: v.numPlayers,
    powerSuit: v.powerSuit,
    firstAttacker: v.firstAttacker,
    defender: v.defender,
    discard: v.discard,
    deckLen: v.deckLen,
    goodMask: v.goodMask,
    hasGoodTs: v.hasGoodTs,
    flipped: v.flipped ? { s: v.flipped.suit, v: v.flipped.value } : null,
    battles: v.battles.map((b) => ({
        attack: { s: b.attack.suit, v: b.attack.value },
        defense: b.defense ? { s: b.defense.suit, v: b.defense.value } : null,
    })),
    elimination: v.elimination,
    // The mirror parses a masked card to null; a real one to a Card.
    hands: v.players.map((p) => p.hand.map((c) => (c ? { s: c.suit, v: c.value } : null))),
    statuses: v.players.map((p) => p.status),
    awaiting: v.players.map((p) => p.awaiting),
});

const fromKernel = (k: KernelState) => ({
    status: k.status,
    numPlayers: k.numPlayers,
    powerSuit: k.powerSuit,
    firstAttacker: k.firstAttacker,
    defender: k.defender,
    discard: k.discardCount,
    deckLen: k.deckCount,
    goodMask: k.goodMask,
    hasGoodTs: k.hasGoodTs,
    flipped: k.flipped,
    battles: k.battles,
    elimination: k.eliminationOrder,
    // The kernel emits "hand":null for a seat that is not the viewer; the mirror
    // emits that seat's cards as an array of nulls. Same claim, said differently:
    // N cards, none of them known.
    hands: k.players.map((p) => (p.hand === null
        ? new Array(p.handCount).fill(null)
        : p.hand.map((c) => ({ s: c.s, v: c.v })))),
    statuses: k.players.map((p) => p.status),
    awaiting: k.players.map((p) => p.awaitingAttack),
});

test('the kernel and the TS mirror read a packed evwire frame identically', async () => {
    await ensureBotsAsync();
    const seeded = await playSeededV6(3, 71, 'robusta');
    assert.ok(seeded, 'the seeded game plays and encodes');

    let frames = 0, events = 0, boards = 0;
    // Every seat and the spectator: masking is per-viewer, so a decoder that is
    // right for one viewer can still be wrong for another.
    for (const viewer of [-1, 0, 1, 2]) {
        const packed = replayEventFrames(seeded!.code, viewer);
        assert.ok(packed.length > 0, `viewer ${viewer} gets frames`);

        for (const frame of packed) {
            const k = kernelEventsFromPacked(frame);
            frames++;

            assert.equal(k.viewer, viewer, 'the frame header round-trips its viewer');

            // The mirror's own read of the same bytes, field by field.
            let q = 4;
            const nEvents = frame[3];
            assert.equal(k.events.length, nEvents, 'the kernel reports every event on the wire');

            for (let i = 0; i < nEvents; i++) {
                const type = frame[q], seat = frame[q + 1], msg = frame[q + 2];
                const from = frame[q + 3], to = frame[q + 4], flags = frame[q + 5];
                const nCards = frame[q + 6];
                q += 7 + nCards + (flags & 1 ? 1 : 0) + (flags & 2 ? 1 : 0);
                const snapLen = frame[q] | (frame[q + 1] << 8); q += 2;

                const ev = k.events[i];
                assert.equal(ev.type, type, 'event type');
                assert.equal(ev.seat, seat === 0xff ? -1 : seat, 'event seat');
                assert.equal(ev.msg, msg, 'message template code');
                assert.equal(ev.from, from, 'from location');
                assert.equal(ev.to, to, 'to location');
                assert.equal(ev.cards.length, nCards, 'card count');
                assert.equal(ev.battle !== undefined, !!(flags & 2), 'battle index presence');
                assert.equal(ev.target !== undefined, !!(flags & 1), 'target presence');

                // The per-step board, both ways.
                assert.deepEqual(fromKernel(ev.state), fromMirror(parseMaskedState(frame, q).state),
                                 'the step board the kernel read is the one the mirror read');
                q += snapLen;
                events++; boards++;
            }

            // The trailer.
            const finLen = frame[q] | (frame[q + 1] << 8); q += 2;
            assert.ok(finLen > 0, 'the frame carries its committed board');
            assert.deepEqual(fromKernel(k.game), fromMirror(parseMaskedState(frame, q).state),
                             'and the committed board agrees too');
            boards++;
        }
    }
    assert.ok(frames > 10, `decoded ${frames} frames`);
    assert.ok(events > 20, `over ${events} events`);
    assert.ok(boards > 30, `and ${boards} boards`);
});

test('the kernel and the TS mirror read a packed view blob identically', async () => {
    await ensureBotsAsync();
    const seeded = await playSeededV6(4, 12, 'robusta');
    assert.ok(seeded, 'the seeded game plays and encodes');

    // A frame's trailer IS a masked view blob of the same layout state_put
    // writes, so it is a real blob to test the view door with.
    const frames = replayEventFrames(seeded!.code, 0);
    const frame = frames[0];
    let q = 4;
    const nEvents = frame[3];
    for (let i = 0; i < nEvents; i++) {
        const flags = frame[q + 5], nCards = frame[q + 6];
        q += 7 + nCards + (flags & 1 ? 1 : 0) + (flags & 2 ? 1 : 0);
        const snapLen = frame[q] | (frame[q + 1] << 8);
        q += 2 + snapLen;
    }
    const finLen = frame[q] | (frame[q + 1] << 8); q += 2;
    const blob = frame.subarray(q, q + finLen);

    assert.deepEqual(fromKernel(kernelViewFromPacked(blob, 0)),
                     fromMirror(parseMaskedState(blob, 0).state),
                     'a packed view blob decodes the same both ways');
});

test('an unreadable payload throws rather than decoding to a wrong board', async () => {
    await ensureBotsAsync();
    const seeded = await playSeededV6(3, 71, 'robusta');
    const frame = replayEventFrames(seeded!.code, -1)[0];

    // Truncation. The mirror returns null here; the kernel door throws. Both
    // refuse — what must never happen is a partial sequence rendered as a whole.
    for (const len of [1, 4, 8, Math.floor(frame.length / 2), frame.length - 1]) {
        assert.throws(() => kernelEventsFromPacked(frame.subarray(0, len)),
                      /not a readable payload|bad argument/,
                      `a ${len}-byte prefix is refused`);
    }

    // A format this build does not read.
    const foreign = frame.slice();
    foreign[0] = 99;
    assert.throws(() => kernelEventsFromPacked(foreign), /not a readable payload/,
                  'a foreign format version is refused, not guessed at');
});
