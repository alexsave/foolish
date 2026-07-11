// E2E: the REAL client reconciliation code (src/state/clientReconcile.ts — the
// exact functions ServerContext/AnimationContext import) exercised directly.
// No React, no port: this is the deployed client logic. Also covers the
// broadcast-reordering convergence that reconcile.test.ts drives end-to-end.
//
// Owns the client-reconcile validation scenarios; the fast runner
// (e2e/validation/client_validation.test.ts) imports `registerClientValidation`.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    displayedHand, reconcileHandMemory, mergeTableBattles, shouldDropStaleSequence, applyOverlayEntries, cardKey,
    reorderHand,
} from '../src/state/clientReconcile';

type C = { suit: number; value: number };
const c = (s: number, v: number): C => ({ suit: s, value: v });
const keys = (cards: C[]) => cards.map(cardKey);
type B = { attack: C; defense: C | null };

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
    test('applyOverlayEntries re-applies unconfirmed optimistic cards onto a resync (no vanish)', () => {
        const myAttack = c(3, 7);
        const game: any = { table_battles: [] as B[], self: { hand: [myAttack, c(0, 5)] } };
        applyOverlayEntries(game, [{ card: myAttack }]);
        assert.ok(game.table_battles.some((b: B) => cardKey(b.attack) === cardKey(myAttack)), 'optimistic attack preserved');
        assert.ok(!game.self.hand.some((x: C) => cardKey(x) === cardKey(myAttack)), 'and removed from hand');

        const atk = c(1, 5), cov = c(0, 9);
        const game2: any = { table_battles: [{ attack: atk, defense: null }] as B[], self: { hand: [cov] } };
        applyOverlayEntries(game2, [{ card: cov, target: atk }]);
        assert.equal(cardKey(game2.table_battles[0].defense), cardKey(cov), 'optimistic cover preserved');
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
}

if (!process.env.VALIDATION_ONLY) registerClientValidation();
