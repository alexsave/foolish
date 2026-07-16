import { Card } from '@shared/core/types.ts';

/**
 * A tiny bridge so the authoritative REST load path (ServerContext.loadGame, used
 * for the reconnect resync) can preserve the local player's *unconfirmed*
 * optimistic table cards.
 *
 * Without this, a resync replaces the table with authoritative server state, which
 * doesn't yet contain a card the player just played — so the card vanishes, then
 * reappears a moment later when its confirming broadcast arrives. AnimationContext
 * registers a provider that reads its live optimistic tracking; the load path
 * re-applies those cards onto the loaded state before committing it.
 *
 * Live broadcasts don't need this — AnimationContext already injects optimistic
 * cards into incoming broadcast states. This only covers the load/resync path.
 */
export interface OptimisticEntry {
    card: Card;            // the optimistically-played card
    target?: Card | null;  // present => it's a cover; the attack card it covers
}

let provider: () => OptimisticEntry[] = () => [];

export const optimisticOverlay = {
    /** AnimationContext registers a getter over its live optimistic tracking.
     *  Returns an unregister fn for effect cleanup. */
    register(fn: () => OptimisticEntry[]): () => void {
        provider = fn;
        return () => { if (provider === fn) provider = () => []; };
    },
    entries(): OptimisticEntry[] {
        try { return provider(); } catch { return []; }
    },
};
