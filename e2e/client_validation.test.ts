// VALIDATION (pure, no Postgres): handpicked regressions for the REAL deployed
// client logic. Small deterministic versions of client.test.ts, reconcile.test.ts
// and optimistic_animation.test.ts — the reconciliation gate/merge, the rendered
// hand, and the optimistic-animation double-play dedup.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { displayedHand, mergeTableBattles, shouldDropStaleSequence, applyOverlayEntries, cardKey } from '../src/state/clientReconcile';
import { createCardEventString, getCardKey } from '../src/utils/animationUtils';
import { staleOptimisticKeysOnTable } from '../src/state/optimisticAnimation';
import { Card } from '../supabase/functions/_shared/types.ts';

type C = { suit: number; value: number };
const c = (s: number, v: number): C => ({ suit: s, value: v });
const keys = (cards: C[]) => cards.map(cardKey);
type B = { attack: C; defense: C | null };

// --- client.test.ts: rendered hand never duplicates / never shows a table card
test('rendered hand: deduped, and a card on the table is never rendered in hand', () => {
    const arranged = [c(0, 5), c(1, 9), c(2, 11)];
    const corrupted = [...arranged, c(2, 11)]; // memory with a duplicate
    assert.equal(new Set(keys(displayedHand(corrupted, arranged))).size, displayedHand(corrupted, arranged).length, 'deduped');
    const onTable = c(2, 11);
    const authHand = arranged.filter((x) => cardKey(x) !== cardKey(onTable));
    assert.ok(!displayedHand(arranged, authHand).some((x) => cardKey(x) === cardKey(onTable)), 'table card not in hand');
});

// --- client.test.ts / reconcile.test.ts: the version gate + trust-incoming merge
test('version gate: drop at-or-below newest applied; replay (no version) never gated', () => {
    assert.equal(shouldDropStaleSequence(5, 4), true, 'older dropped');
    assert.equal(shouldDropStaleSequence(5, 5), true, 'duplicate-version dropped');
    assert.equal(shouldDropStaleSequence(5, 6), false, 'newer applied');
    assert.equal(shouldDropStaleSequence(null, 1), false, 'first applied');
    assert.equal(shouldDropStaleSequence(5, null), false, 'replay never gated');
});

test('reordering: gate + trust-incoming merge land the client on the NEWEST bout, not a stale one', () => {
    // v1 bout A (covered), v2 round-clear, v3 bout B (new attack).
    const stream = [
        { version: 1, finalTable: [{ attack: c(0, 6), defense: c(0, 7) }] as B[] },
        { version: 2, finalTable: [] as B[] },
        { version: 3, finalTable: [{ attack: c(2, 8), defense: null }] as B[] },
    ];
    const tkeys = (bs: B[]) => bs.flatMap((b) => (b.defense ? [cardKey(b.attack), cardKey(b.defense)] : [cardKey(b.attack)])).sort();
    const replay = (order: typeof stream, gated: boolean) => {
        let table: B[] = []; let last: number | null = null;
        for (const b of order) {
            if (gated && shouldDropStaleSequence(last, b.version)) continue;
            table = mergeTableBattles(table, b.finalTable); last = b.version;
        }
        return table;
    };
    const reordered = [stream[2], stream[0], stream[1]]; // newest first, then older
    assert.deepEqual(tkeys(replay(reordered, true)), tkeys(stream[2].finalTable), 'gate => newest bout');
    assert.notDeepEqual(tkeys(replay(reordered, false)), tkeys(stream[2].finalTable), 'no gate => stale (why the gate exists)');
    // disconnect: a dropped round-clear must not strand previous-bout cards.
    assert.deepEqual(tkeys(replay([stream[0], stream[2]], true)), tkeys(stream[2].finalTable), 'trust-incoming replaces, no cross-bout');
});

// --- client.test.ts: optimistic overlay survives a resync (no vanish) ---------
test('optimistic overlay: an unconfirmed attack/cover is re-applied onto a resync (no vanish)', () => {
    const myAttack = c(3, 7);
    const g1: any = { table_battles: [] as B[], self: { hand: [myAttack, c(0, 5)] } };
    applyOverlayEntries(g1, [{ card: myAttack }]);
    assert.ok(g1.table_battles.some((b: B) => cardKey(b.attack) === cardKey(myAttack)), 'optimistic attack preserved');
    assert.ok(!g1.self.hand.some((x: C) => cardKey(x) === cardKey(myAttack)), 'and removed from hand');

    const atk = c(1, 5), cov = c(0, 9);
    const g2: any = { table_battles: [{ attack: atk, defense: null }] as B[], self: { hand: [cov] } };
    applyOverlayEntries(g2, [{ card: cov, target: atk }]);
    assert.equal(cardKey(g2.table_battles[0].defense), cardKey(cov), 'optimistic cover preserved');
});

// --- optimistic_animation.test.ts: the "card animates twice" dedup ------------
test('optimistic animation: the same broadcast that confirms a card does NOT pre-release it (no double-play)', () => {
    const SELF = 'player-self';
    const card: Card = { suit: 1, value: 9 };
    const optimisticKey = createCardEventString('attack_pass', card, 'hand', 'table', SELF);
    const serverEvent = { type: 'attack_pass', player_id: SELF, cards: [card], from_location: 'hand', to_location: 'table' };

    // confirming broadcast: must NOT release the very card it names — dedup handles it.
    assert.deepEqual(staleOptimisticKeysOnTable([optimisticKey], [card], [serverEvent]), [], 'not pre-released');
    const serverKey = createCardEventString(serverEvent.type, serverEvent.cards[0], serverEvent.from_location, serverEvent.to_location, serverEvent.player_id);
    assert.ok(new Set([optimisticKey]).has(serverKey), 'server event still matches the optimistic key (so it is skipped)');

    // dropped-broadcast safety net: a later broadcast not naming our card releases it.
    const unrelated = { type: 'cover', cards: [{ suit: 2, value: 10 }], from_location: 'hand', to_location: 'table' };
    assert.deepEqual(staleOptimisticKeysOnTable([optimisticKey], [card], [unrelated]), [optimisticKey], 'lingering entry released');
    // not yet on the authoritative table -> keep it.
    assert.deepEqual(staleOptimisticKeysOnTable([optimisticKey], [], []), [], 'kept until on table');
    assert.equal(getCardKey(JSON.parse(optimisticKey).card), getCardKey(card));
});
