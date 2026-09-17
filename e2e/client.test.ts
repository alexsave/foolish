// E2E: the REAL client reconciliation code (src/state/clientReconcile.ts and the
// kernel-made boards in src/state/clientBoards.ts - the exact functions
// ServerContext/AnimationContext import) exercised directly.
// No React, no port: this is the deployed client logic. Also covers the
// broadcast-reordering convergence that reconcile.test.ts drives end-to-end.
//
// Owns the client-reconcile validation scenarios; the fast runner
// (e2e/validation/client_validation.test.ts) imports `registerClientValidation`.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    displayedHand, reconcileHandMemory, mergeTableBattles, shouldDropStaleSequence, cardKey,
    reorderHand,
} from '../src/state/clientReconcile';
import { keepPending, rearrangedBoard } from '../src/state/clientBoards';
import type { TableView } from '../sdk/ts/table/client_table.ts';
import * as V from '../sdk/ts/gen/view_layout.bots.ts';

type C = { suit: number; value: number };
const c = (s: number, v: number): C => ({ suit: s, value: v });
const keys = (cards: C[]) => cards.map(cardKey);
type B = { attack: C; defense: C | null };
const NONE: C = { suit: V.CARD_NONE_SUIT, value: V.CARD_NONE_VALUE };

// A 2-seat board the web holds (TableView): seat 1 leads, seat 0 defends; the
// viewer (seat 0 unless said) holds `hand`, the table is `battles`.
const board = (battles: B[], hand: C[], mySeat = 0): TableView => ({
    status: V.GAME_STATUS_PLAYING, powerSuit: 2, firstAttacker: 1, defender: 0, mySeat, fool: -1,
    deckCount: 0, discardPileLength: 0, hasFlipped: false, hasGoodTimestamp: false, flipped: NONE, goodMask: 0, version: 3,
    battles: battles.map((b) => ({ attack: b.attack, defense: b.defense ?? NONE })),
    seats: [
        { status: V.PLAYER_STATUS_IN, handCount: mySeat === 0 ? hand.length : 5, awaitingAttack: false, isAi: false, id: 'p0', name: 'P0' },
        { status: V.PLAYER_STATUS_IN, handCount: mySeat === 1 ? hand.length : 5, awaitingAttack: false, isAi: false, id: 'p1', name: 'P1' },
    ],
    myHand: mySeat >= 0 ? hand : [], elimination: [], gameId: 'g', title: 'g',
});

export function registerClientValidation(): void {
    // ---- hand order (crazy swaps / duplicates / in-hand+on-table) --------------
    test('rendered hand: a rejected card keeps its slot (no crazy swap)', () => {
        const arranged = [c(0, 5), c(1, 9), c(2, 11), c(3, 6), c(0, 13)];
        let memory = reconcileHandMemory([], arranged);
        // optimistically play the middle card -> gone from authoritative hand
        const authWhilePlaying = arranged.filter((x) => cardKey(x) !== cardKey(c(2, 11)));
        memory = reconcileHandMemory(memory, authWhilePlaying);
        assert.deepEqual(keys(displayedHand(memory, authWhilePlaying)), keys(authWhilePlaying), 'hidden while in flight');
        // rejected -> card returns to the authoritative hand
        memory = reconcileHandMemory(memory, arranged);
        assert.deepEqual(keys(displayedHand(memory, arranged)), keys(arranged), 'returns to ORIGINAL slot, not the end');
    });

    test('rendered hand: never duplicates and never shows a card that is on the table', () => {
        const arranged = [c(0, 5), c(1, 9), c(2, 11)];
        // corrupted memory with a duplicate
        assert.equal(new Set(keys(displayedHand([...arranged, c(2, 11)], arranged))).size, displayedHand([...arranged, c(2, 11)], arranged).length, 'deduped');
        // a card on the table (absent from the authoritative hand) is never rendered
        const onTable = c(2, 11);
        const authHand = arranged.filter((x) => cardKey(x) !== cardKey(onTable));
        assert.ok(!displayedHand(arranged, authHand).some((x) => cardKey(x) === cardKey(onTable)), 'table card not in hand');
    });

    // ---- table merge (cross-bout fix) ------------------------------------------
    test('mergeTableBattles trusts the incoming authoritative table (no stale-bout append)', () => {
        const boutA: B[] = [{ attack: c(0, 6), defense: c(0, 7) }];
        const boutB: B[] = [{ attack: c(2, 8), defense: null }];
        assert.deepEqual(mergeTableBattles(boutA, boutB), boutB, 'previous bout is not re-appended');
        assert.deepEqual(mergeTableBattles(boutA, []), [], 'empty incoming clears');
    });

    // ---- version gate (reordering fix) -----------------------------------------
    test('shouldDropStaleSequence drops at-or-below the newest applied version, keeps replay (no version)', () => {
        assert.equal(shouldDropStaleSequence(5, 4), true, 'older dropped');
        assert.equal(shouldDropStaleSequence(5, 5), true, 'duplicate-version dropped');
        assert.equal(shouldDropStaleSequence(5, 6), false, 'newer applied');
        assert.equal(shouldDropStaleSequence(null, 1), false, 'first applied');
        assert.equal(shouldDropStaleSequence(5, null), false, 'replay (no version) never gated');
    });

    // ---- reordering / cross-bout (codified from the latency sweeps; reconcile.test.ts)
    // A stream spanning a bout change: v1 = bout A (covered), v2 = clear, v3 = bout B.
    const boutStream = [
        { version: 1, finalTable: [{ attack: c(0, 6), defense: c(0, 7) }] as B[] }, // bout A, covered
        { version: 2, finalTable: [] as B[] },                                       // round-transition clear
        { version: 3, finalTable: [{ attack: c(2, 8), defense: null }] as B[] },     // bout B, new attack
    ];
    const replay = (order: typeof boutStream, gated: boolean) => {
        let table: B[] = []; let last: number | null = null;
        for (const b of order) {
            if (gated && shouldDropStaleSequence(last, b.version)) continue;
            table = mergeTableBattles(table, b.finalTable);
            last = b.version;
        }
        return table;
    };
    const tkeys = (bs: B[]) => bs.flatMap((b) => (b.defense ? [cardKey(b.attack), cardKey(b.defense)] : [cardKey(b.attack)])).sort();

    test('reordering: the version gate lands the client on the newest bout (not a stale one)', () => {
        // adversarial: newest (v3) arrives first, then the older v1 and the clear v2.
        const reordered = [boutStream[2], boutStream[0], boutStream[1]];
        assert.deepEqual(tkeys(replay(reordered, true)), tkeys(boutStream[2].finalTable), 'gate => newest bout');
        // without the gate the client would end on whatever arrived last (a stale bout)
        assert.notDeepEqual(tkeys(replay(reordered, false)), tkeys(boutStream[2].finalTable), 'no gate => stale (why the gate is needed)');
    });

    test('disconnect: a dropped round-transition clear does not strand previous-bout cards', () => {
        // the clear (v2) is the lost packet; the client jumps A -> B directly.
        const dropped = [boutStream[0], boutStream[2]];
        assert.deepEqual(tkeys(replay(dropped, true)), tkeys(boutStream[2].finalTable), 'trust-incoming replaces, no cross-bout cards');
    });

    // ---- optimistic overlay (resync no-vanish) ---------------------------------
    test('keepPending re-applies unconfirmed optimistic cards onto a resync (no vanish)', () => {
        const myAttack = c(3, 7);
        const game = board([], [myAttack, c(0, 5)]);
        const after = keepPending(game, [{ card: myAttack }])!;
        assert.ok(after.battles.some((b) => cardKey(b.attack) === cardKey(myAttack)), 'optimistic attack preserved');
        assert.ok(!after.myHand.some((x: C) => cardKey(x) === cardKey(myAttack)), 'and removed from hand');
        assert.equal(game.battles.length, 0, 'the held board is not changed in place');
        assert.equal(keepPending(after, [{ card: myAttack }])!.battles.length, 1, 'idempotent: a card the table holds is not laid twice');

        const atk = c(1, 5), cov = c(0, 9);
        const game2 = board([{ attack: atk, defense: null }], [cov]);
        const after2 = keepPending(game2, [{ card: cov, target: atk }])!;
        assert.equal(cardKey(after2.battles[0].defense), cardKey(cov), 'optimistic cover preserved');
        assert.deepEqual(keepPending(board([{ attack: atk, defense: null }], [], -1), [{ card: cov, target: atk }])!.battles[0].defense, NONE,
            'a spectator has nothing optimistic to re-apply');
    });

    // ---- drag-rearrange bounds safety (regression: prod "undefined is not an
    // object (evaluating 'e.suit')") ------------------------------------------
    // The DragContext swap read a hovered card's DOM `data-card-index` and did
    // `next[toIndex] = dragged`. When the hand shrank mid-drag the stale index
    // outran the array, creating a SPARSE hole that crashed the hand render's
    // cardKey/.map. reorderHand is the deployed fix.
    test('reorderHand: an out-of-bounds target is a no-op (never makes an undefined hole)', () => {
        const hand = [c(0, 6), c(1, 7), c(2, 8), c(3, 9), c(0, 10), c(1, 11)]; // length 6

        // Demonstrate the ORIGINAL bug shape: the naive swap makes a hole and the
        // render map then throws exactly the production error.
        const naive = [...hand];
        const dragged = naive[2];
        naive[2] = naive[8];   // undefined
        naive[8] = dragged;    // extend past end -> holes at 6,7
        assert.equal(naive.length, 9, 'naive swap corrupted the array length');
        assert.throws(() => naive.map(cardKey), /suit/, 'naive swap crashes the render map (the prod bug)');

        // The fix: a stale/out-of-range target index leaves the hand untouched,
        // and returns the SAME reference so DragContext treats it as a no-op.
        for (const bad of [6, 8, 99, -1, NaN]) {
            const out = reorderHand(hand, 2, bad);
            assert.equal(out, hand, `toIndex=${bad} must be a no-op (same reference)`);
        }
        assert.equal(reorderHand(hand, 9, 2), hand, 'out-of-range fromIndex is also a no-op');

        // A valid in-range swap still works and never produces undefined.
        const swapped = reorderHand(hand, 1, 4);
        assert.equal(swapped.length, 6);
        assert.deepEqual(keys(swapped), keys([c(0, 6), c(0, 10), c(2, 8), c(3, 9), c(1, 7), c(1, 11)]));
        assert.ok(swapped.every((x) => x != null), 'no undefined slots');
        assert.doesNotThrow(() => swapped.map(cardKey), 'render map never throws on a reordered hand');
    });

    // ---- debounced rearrange-flush safety (same 'e.suit' crash, other path) ---
    // ServerContext.rearrangeHand orders the hand optimistically. The indices are
    // computed at drag-end but the flush is debounced ~5s; if the hand shrinks
    // first (a card played/drawn/picked up) an index outruns the now-shorter hand,
    // and a naive `indices.map(i => hand[i])` mints an undefined slot the hand
    // render's cardKey/.map crashes on. The kernel orders a hand only by a true
    // permutation (clientBoards.rearrangedBoard -> game_rearrange_hand), so a stale
    // reorder is abandoned instead of applied.
    test('rearrangedBoard: stale/out-of-range indices are not an order (no undefined hole)', () => {
        const hand = [c(0, 6), c(1, 7), c(2, 8), c(3, 9), c(0, 10)]; // length 5
        const held = board([], hand);

        // The prod crash shape: indices captured against a length-6 hand, applied
        // after it shrank to 5 -> index 5 is out of range -> undefined slot.
        const stale = [0, 1, 2, 3, 5];
        const naive = stale.map((i) => hand[i]);
        assert.ok(naive.includes(undefined as never), 'stale index minted an undefined slot');
        assert.throws(() => naive.map(cardKey), /suit/, 'the holed hand crashes the render map (the prod bug)');
        assert.equal(rearrangedBoard(held, stale).kind, 'not-an-order', 'out-of-range index rejected');

        // Other degenerate shapes the kernel refuses.
        assert.equal(rearrangedBoard(held, [0, 0, 1, 2, 3]).kind, 'not-an-order', 'duplicate indices rejected');
        assert.equal(rearrangedBoard(held, [0, 1, 2, 3]).kind, 'not-an-order', 'wrong length rejected');
        assert.equal(rearrangedBoard(held, [0, 1, 2, 3, NaN]).kind, 'not-an-order', 'NaN rejected');
        assert.equal(rearrangedBoard(held, [-1, 1, 2, 3, 4]).kind, 'not-an-order', 'negative index rejected');
        assert.equal(rearrangedBoard(held, [0, 1, 2, 3, 4.5]).kind, 'not-an-order', 'a fractional index rejected');
        assert.equal(rearrangedBoard(board([], []), []).kind, 'no-hand', 'an empty hand has nothing to order');
        assert.equal(rearrangedBoard(board([], [], -1), []).kind, 'no-hand', 'nor has a spectator');

        // A genuine permutation orders the hand (never undefined).
        const good = [4, 3, 2, 1, 0];
        const ordered = rearrangedBoard(held, good);
        assert.equal(ordered.kind, 'ordered', 'a real permutation is accepted');
        const applied = ordered.kind === 'ordered' ? ordered.view.myHand : [];
        assert.deepEqual(keys([...applied]), keys(good.map((i) => hand[i])), 'the hand takes the order the indices give');
        assert.doesNotThrow(() => applied.map(cardKey), 'render map never throws on a valid rearrange');
    });
}

if (!process.env.VALIDATION_ONLY) registerClientValidation();
