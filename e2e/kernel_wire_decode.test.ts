/* =============================================================================
 * The wire decode says what the engine actually did (A8/F7, A9)
 * =============================================================================
 * This file began as a twin test: the kernel's reader vs the TypeScript mirror
 * that shadowed view.c and evwire.c byte for byte. That mirror is deleted, so
 * there is no second implementation left to agree with — and agreeing with a
 * copy was always the weaker claim anyway. It could only ever say "the two
 * match", never "the answer is right" (docs/C_CORE_CONSOLIDATION.md A9).
 *
 * What it asserts now is the strong version: decode the frames and you get back
 * the game the ENGINE PLAYED. The oracle is a real seeded game, played by the
 * real engine, encoded by the real codec — not a frozen fixture, which in this
 * repo only ever means a fixture that has quietly rotted (see helpers/seeded_game.ts).
 *
 * The mirror-vs-kernel comparison it used to do lives on in the commit history
 * (bae4093), which is where a cutover harness belongs once the cutover is made.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    ensureBotsAsync, kernelEventsFromPacked, replayEventFrames,
} from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { decodeEventWire } from '../supabase/functions/_shared/sdk/ts/wire/evwire.ts';
import { PLAYER_STATUS } from '../supabase/functions/_shared/types.ts';
import { playSeededV6 } from './helpers/seeded_game.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const roster = (np: number) => ({
    id: 'g', name: 'g',
    players: Array.from({ length: np }, (_, i) => ({
        player_id: `seat-${i}`, name: `P${i}`, is_ai: true,
    })),
});
const CTX = { preGood: [] as string[], prevGoodTs: null, now: () => 0 };

// The last frame of a finished game carries the committed final board as its
// trailer. That board must be the one the engine ended on — the whole decode
// path (kernel reader + host roster join) held against the real thing.
test('the decoded final board is the board the engine actually ended on', async () => {
    await ensureBotsAsync();
    for (const [np, seed] of [[2, 5], [3, 71], [4, 12]] as const) {
        const seeded = await playSeededV6(np, seed, 'robusta');
        assert.ok(seeded, `${np}p seeded game plays and encodes`);
        const played = seeded!.game;

        const frames = replayEventFrames(seeded!.code, -1);
        const last = decodeEventWire(frames[frames.length - 1], roster(np), CTX);
        assert.ok(last, 'the last frame decodes');

        const board = last!.game;
        assert.equal(board.deck_length, played.deck.length, `${np}p: the deck the engine left`);
        assert.equal(board.discard_pile_length, played.discard_pile_length, `${np}p: the discard`);
        assert.equal(board.power_suit, played.power_suit, `${np}p: the trump`);
        assert.equal(board.table_battles.length, played.table_battles.length, `${np}p: the table`);
        assert.deepEqual(board.players.map((p) => p.hand_length),
                         played.players.map((p) => p.hand.length),
                         `${np}p: every seat's hand size`);
        assert.deepEqual(board.players.map((p) => p.status),
                         played.players.map((p) => p.status),
                         `${np}p: who was in, out, and the fool`);
        assert.equal(board.players.filter((p) => p.status === PLAYER_STATUS.OUT).length,
                     np - 1, `${np}p: everyone but the fool is out`);
    }
});

// Masking is per-viewer and the kernel does it (view.c), but a decoder that
// dropped it on the floor would still produce a plausible-looking board. A
// spectator must see no card identities at all; the seat itself must see its own.
test('a spectator decode carries no hand, and a seat decode carries its own', async () => {
    await ensureBotsAsync();
    const seeded = await playSeededV6(3, 71, 'robusta');
    const code = seeded!.code;

    // The deal frame — the one place every hand is full and a leak would show.
    const spectator = decodeEventWire(replayEventFrames(code, -1)[0], roster(3), CTX);
    assert.ok(spectator, 'the deal frame decodes for a spectator');
    assert.equal((spectator!.game as { self?: unknown }).self, undefined,
                 'a spectator has no self');

    // DEAL and REFILL are the redacted types: a card bound for someone's hand.
    // The FLIPPED trump is public by the rules of the game and must NOT be a
    // back — asserting "every card is a back" would have been wrong, and was:
    // measured, the deal frame is 3 fully-masked DEALs plus one real trump.
    let dealt = 0, flipped = 0;
    for (const ev of spectator!.events) {
        const cards = ev.cards ?? [];
        if (ev.type === 'deal' || ev.type === 'refill') {
            for (const c of cards) {
                assert.ok(c.suit === -1 && c.value === -1,
                          'a card dealt to a hand is a back for a spectator');
            }
            dealt += cards.length;
        }
        if (ev.type === 'flipped') {
            for (const c of cards) assert.ok(c.suit >= 0, 'the trump is public');
            flipped += cards.length;
        }
    }
    assert.ok(dealt > 0, `${dealt} dealt cards were checked — the loop is not vacuous`);
    assert.equal(flipped, 1, 'and exactly one trump was turned');

    const seat1 = decodeEventWire(replayEventFrames(code, 1)[0], roster(3), CTX);
    assert.ok(seat1, 'the deal frame decodes for seat 1');
    const self = (seat1!.game as { self: { hand: { suit: number }[] } }).self;
    assert.ok(self, 'seat 1 has a self');
    assert.ok(self.hand.length > 0, 'holding cards');
    assert.ok(self.hand.every((c) => c.suit >= 0), "and seat 1's own cards are face-up");

    // The seat sees its OWN dealt cards, and no one else's: the DEAL events
    // aimed at other seats stay backs even in seat 1's own stream.
    const ownDeal = seat1!.events.some((e) => (e.cards ?? []).some((c) => c.suit >= 0));
    assert.ok(ownDeal, 'seat 1 watches its own cards arrive');
});

// An unreadable payload must refuse. Half a sequence rendered as a whole one is
// worse than no sequence: the board would silently be wrong.
test('an unreadable payload decodes to null rather than a wrong board', async () => {
    await ensureBotsAsync();
    const seeded = await playSeededV6(3, 71, 'robusta');
    const frame = replayEventFrames(seeded!.code, -1)[0];

    assert.ok(decodeEventWire(frame, roster(3), CTX), 'the whole frame reads (the control)');

    for (const len of [1, 4, 8, Math.floor(frame.length / 2), frame.length - 1]) {
        assert.equal(decodeEventWire(frame.subarray(0, len), roster(3), CTX), null,
                     `a ${len}-byte prefix is unreadable, not a partial board`);
    }

    const foreign = frame.slice();
    foreign[0] = 99;
    assert.equal(decodeEventWire(foreign, roster(3), CTX), null,
                 'a format this build does not read is refused, not guessed at');

    // And the raw door says why, rather than returning an empty sequence.
    assert.throws(() => kernelEventsFromPacked(foreign), /not a readable payload/);
});
