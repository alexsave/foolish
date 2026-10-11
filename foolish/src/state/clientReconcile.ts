// Pure client-side reconciliation logic, extracted from ServerContext /
// AnimationContext so the SAME deployed code can be unit-tested directly (no React
// needed). The components import from here; the e2e suite imports from here. There
// is no second copy. The boards the client makes itself - an optimistic move, a
// resync that keeps pending cards, the rematch's lobby - are the kernel's
// (src/state/clientBoards.ts), and a push is gated by the kernel's version gate
// (anim_plan.h anim_should_drop_stale, asked in AnimationContext); what is left
// here orders a hand.

import type { ViewCard } from './view';

type Card = ViewCard;

export const cardKey = (c: Card): string => `${c.suit}-${c.value}`;

// ---- Hand reconciliation ---------------------------------------------------
// Legacy order-preserving merge (kept for game.self.hand). Appends new cards at
// the end; superseded for the RENDERED hand by reconcileHandMemory + displayedHand.
export const mergeHandOrder = (oldHand: readonly Card[], newHand: readonly Card[]): Card[] => {
    if (!oldHand || !newHand) return [...(newHand || [])];
    const oldKeys = new Set(oldHand.map(cardKey));
    const newKeys = new Set(newHand.map(cardKey));
    const preserved = oldHand.filter((c) => newKeys.has(cardKey(c)));
    const added = newHand.filter((c) => !oldKeys.has(cardKey(c)));
    return [...preserved, ...added];
};

// Bounds-safe drag-rearrange swap (DragContext). `toIndex` comes from a DOM
// `data-card-index` read during a mousemove; if the hand shrinks between render
// and that read (a card resolves / an opponent acts / a resync mid-drag), the
// stale index can outrun `order`. The naive `next[toIndex] = ...` then produced
// a SPARSE array with `undefined` holes, which crashed the hand render's
// `cardKey`/`.map` on `card.suit` (prod: "undefined is not an object
// (evaluating 'e.suit')"). Any out-of-range / degenerate move returns the input
// array unchanged (referential identity signals "no-op" to the caller), so a
// hole can never be created.
export const reorderHand = (order: readonly Card[], fromIndex: number, toIndex: number): readonly Card[] => {
    // Number.isInteger rejects NaN (parseInt on a missing/garbled data-card-index)
    // and floats — NaN would slip past `< 0` / `>= length` (both false for NaN).
    if (!order
        || fromIndex === toIndex
        || !Number.isInteger(fromIndex) || fromIndex < 0 || fromIndex >= order.length
        || !Number.isInteger(toIndex) || toIndex < 0 || toIndex >= order.length) {
        return order;
    }
    const next = [...order];
    const moved = next[fromIndex];
    next[fromIndex] = next[toIndex];
    next[toIndex] = moved;
    return next;
};

// Sticky arrangement memory: keeps every known card's slot and only grows with
// genuinely-new cards, so a card removed optimistically and then rejected keeps
// its slot instead of jumping to the end.
export const reconcileHandMemory = (memory: readonly Card[], authHand: readonly Card[]): Card[] => {
    const seen = new Set<string>();
    const dedupMem = (memory || []).filter((c) => { const k = cardKey(c); if (seen.has(k)) return false; seen.add(k); return true; });
    const additions = (authHand || []).filter((c) => !seen.has(cardKey(c)));
    return [...dedupMem, ...additions];
};

// The rendered hand: the authoritative hand, deduplicated and ordered by the
// memory. A card not in the authoritative hand (played, or on the table) is never
// shown; duplicates are impossible by construction.
export const displayedHand = (memory: readonly Card[], authHand: readonly Card[]): Card[] => {
    const byKey = new Map((authHand || []).map((c) => [cardKey(c), c] as const));
    const used = new Set<string>();
    const out: Card[] = [];
    for (const m of (memory || [])) { const k = cardKey(m); const a = byKey.get(k); if (a && !used.has(k)) { out.push(a); used.add(k); } }
    for (const c of (authHand || [])) { const k = cardKey(c); if (!used.has(k)) { out.push(c); used.add(k); } }
    return out;
};
