import { Card } from '@api/core/types.ts';
import { getCardKey } from '../utils/animationUtils';

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
 * Pure: returns the optimistic-map keys to release; no side effects.
 */
export function staleOptimisticKeysOnTable(
    optimisticKeys: Iterable<string>,
    tableCards: Card[],
    events: AnimEvent[],
): string[] {
    const onTable = new Set(tableCards.map(getCardKey));
    const namedByThisBroadcast = new Set<string>();
    for (const ev of events) {
        for (const c of ev.cards ?? []) namedByThisBroadcast.add(getCardKey(c));
    }

    const release: string[] = [];
    for (const key of optimisticKeys) {
        let card: Card | undefined;
        try {
            card = JSON.parse(key).card;
        } catch {
            continue; // malformed key — leave it
        }
        if (!card) continue;
        const ck = getCardKey(card);
        if (onTable.has(ck) && !namedByThisBroadcast.has(ck)) {
            release.push(key);
        }
    }
    return release;
}
