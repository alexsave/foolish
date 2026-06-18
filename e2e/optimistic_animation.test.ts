/* =============================================================================
 * Optimistic-animation dedup regression test (the "card animates twice" bug)
 * =============================================================================
 * Exercises the REAL client helpers: the same createCardEventString the
 * AnimationContext uses to key optimistic animations, and the real
 * staleOptimisticKeysOnTable the version-gate uses to release them.
 *
 * The bug: when you play a card, the optimistic animation plays, then the
 * server's confirming broadcast plays it AGAIN. Cause — the version gate
 * released the optimistic entry for any card now on the authoritative table,
 * INCLUDING the card the very same broadcast was confirming, so the per-event
 * dedup downstream no longer recognised it and re-animated it.
 *
 * Pure logic — no Postgres, no harness.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Card } from '../supabase/functions/_shared/types.ts';
import { createCardEventString, getCardKey } from '../src/utils/animationUtils';
import { staleOptimisticKeysOnTable } from '../src/state/optimisticAnimation';

const SELF = 'player-self';
const card: Card = { suit: 1, value: 9 };

// How AnimationContext keys an optimistic attack: ('attack_pass', card, hand->table, self)
const optimisticAttackKey = createCardEventString('attack_pass', card, 'hand', 'table', SELF);

// The server's confirming attack broadcast (supabase/_shared/actions/attack.ts).
const serverAttackEvent = {
    type: 'attack_pass',
    player_id: SELF,
    cards: [card],
    from_location: 'hand',
    to_location: 'table',
};

// The authoritative game state the broadcast carries: the card is now on table.
const tableCardsAfter: Card[] = [card];

test('version gate does NOT release an optimistic card the same broadcast confirms (no double-play)', () => {
    const release = staleOptimisticKeysOnTable([optimisticAttackKey], tableCardsAfter, [serverAttackEvent]);
    assert.deepEqual(release, [], 'must not pre-release a card named by this broadcast — the dedup handles it');

    // …and because it was NOT released, the per-event dedup still recognises it
    // (this is the exact match AnimationContext does), so the server event is
    // skipped instead of animating a second time.
    const optimisticKeys = new Set([optimisticAttackKey]);
    const serverKey = createCardEventString(
        serverAttackEvent.type,
        serverAttackEvent.cards[0],
        serverAttackEvent.from_location,
        serverAttackEvent.to_location,
        serverAttackEvent.player_id,
    );
    assert.ok(optimisticKeys.has(serverKey), 'server confirming event must still match the optimistic key');
});

test('version gate DOES release an on-table optimistic card whose confirming broadcast was dropped', () => {
    // A later-versioned broadcast that does NOT name our card (its own confirming
    // broadcast was reordered/dropped by the gate) still shows it on the table.
    const unrelatedEvent = { type: 'cover', cards: [{ suit: 2, value: 10 }], from_location: 'hand', to_location: 'table' };
    const release = staleOptimisticKeysOnTable([optimisticAttackKey], tableCardsAfter, [unrelatedEvent]);
    assert.deepEqual(release, [optimisticAttackKey], 'must release the lingering optimistic entry (dropped-broadcast safety net)');
});

test('version gate leaves optimistic cards that are not yet on the authoritative table', () => {
    const release = staleOptimisticKeysOnTable([optimisticAttackKey], [], []);
    assert.deepEqual(release, [], 'nothing on table yet — keep the optimistic entry');
    // sanity: the key really is for our card
    assert.equal(getCardKey(JSON.parse(optimisticAttackKey).card), getCardKey(card));
});
