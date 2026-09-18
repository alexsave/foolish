// Pure client-side reconciliation logic, extracted from ServerContext /
// AnimationContext so the SAME deployed code can be unit-tested directly (no React
// needed). The components import from here; the e2e suite imports from here. There
// is no second copy. The boards the client makes itself - an optimistic move, a
// resync that keeps pending cards, the rematch's lobby - are the kernel's
// (src/state/clientBoards.ts); what is left here orders a hand and gates a push.

import { animShouldDropStale } from '@sdk/ts/wasm/bots.ts';
import type { ViewCard } from './view';

type Card = ViewCard;

export const cardKey = (c: Card): string => `${c.suit}-${c.value}`;

// ---- Live broadcast ordering gate -----------------------------------------
// Broadcasts are fired un-awaited over per-call channels, so under realtime
// latency they can arrive out of order. Each carries the committed games.version;
// drop any whose version is at or below the newest already applied (it's strictly
// superseded — each sequence carries the full resulting state). Replay sequences
// have no version and are never gated.
// Delegates to the C animation core (c/src/anim_plan.h anim_should_drop_stale)
// through the wasm bridge, so the gate that decides which broadcast every client
// applies lives in ONE place — a phone and a browser disagreeing here forks the
// feed. The C twin is asserted natively (c/tests/anim_plan_test.c test_reconcile)
// AND exercised end-to-end through this delegation by e2e/reconcile.test.ts.
export const shouldDropStaleSequence = (lastAppliedVersion: number | null, incomingVersion: number | null): boolean =>
    animShouldDropStale(lastAppliedVersion, incomingVersion);

// ---- Table reconciliation --------------------------------------------------
// Trust the server's table outright. Ordering is handled by the version gate and
// the local player's unconfirmed cards are injected upstream (optimistic resolver
// for broadcasts, clientBoards.keepPending for resync), so appending stale leftover
// battles is unnecessary and would re-introduce a previous bout's cards when an
// intermediate clear is skipped.
//
// This is the C plan's "trust incoming" merge policy expressed at the JS-null
// boundary — there is no decision for the kernel to make (no branch, no compute),
// only a null-coalesce over Battle[] objects the C side never needs to see. It
// stays here deliberately; the animation-core DECISIONS (the gate above, the
// optimistic resolver, the plan) all delegate to C.
export const mergeTableBattles = <B>(existingBattles: readonly B[] | undefined, incomingBattles: readonly B[] | undefined): readonly B[] => {
    return incomingBattles ?? existingBattles ?? [];
};

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
