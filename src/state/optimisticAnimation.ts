import type { ViewCard as Card } from './view';
import { animStaleOptimisticOnTable } from '@sdk/ts/wasm/bots.ts';

interface AnimEvent {
    cards?: Card[];
    [k: string]: unknown;
}

/**
 * When an authoritative versioned broadcast lands, the live-game animation
 * handler releases optimistic-animation entries for any of the player's cards
 * the server's table now shows. But it must release ONLY the cards whose
 * confirming broadcast was DROPPED by the version gate — i.e. cards NOT named
 * by this broadcast's own events.
 *
 * Cards this broadcast *does* name are left alone here on purpose: the dedup
 * partition downstream matches them against the optimistic map and skips
 * re-animating them. Releasing them here first would make their own confirming
 * event look un-optimistic, so it would animate a SECOND time — the "I played
 * one card but it animates twice" bug.
 *
 * The DECISION lives in the C animation core (c/src/anim_plan.h
 * anim_stale_optimistic_on_table), reached through the wasm bridge, so the web,
 * iOS and any future client share one implementation. This wrapper is the
 * marshal and nothing else: it takes the pending map's entries — each a key and
 * the card it was predicted for — hands the cards to C, and maps the returned
 * indices back to the keys to release. It never reads a key, because a key is
 * the kernel's (anim_event_key) and has no fields a host may take apart.
 * Asserted natively (c/tests/anim_plan_test.c test_optimistic_animation) and
 * end-to-end via e2e/optimistic_animation.test.ts.
 *
 * Pure: returns the optimistic-map keys to release; no side effects.
 */
export function staleOptimisticKeysOnTable<K>(
    pending: Iterable<readonly [K, { card: Card }]>,
    tableCards: Card[],
    events: AnimEvent[],
): K[] {
    const keys: K[] = [];
    const optCards: Card[] = [];
    for (const [key, motion] of pending) {
        keys.push(key);
        optCards.push(motion.card);
    }

    const named: Card[] = [];
    for (const ev of events) for (const c of ev.cards ?? []) named.push(c);

    return animStaleOptimisticOnTable(optCards, tableCards, named).map((i) => keys[i]);
}
