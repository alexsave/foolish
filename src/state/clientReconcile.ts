// Pure client-side reconciliation logic, extracted from ServerContext /
// AnimationContext so the SAME deployed code can be unit-tested directly (no React
// needed). The components import from here; the e2e suite imports from here. There
// is no second copy.

import { animShouldDropStale } from '@sdk/ts/wasm/bots.ts';
import { GAME_STATUS, NO_CARD, PLAYER_STATUS, covered, sameCard, type TableView, type ViewCard } from './view';

type Card = ViewCard;

export const cardKey = (c: Card): string => `${c.suit}-${c.value}`;

// The lobby a finished game resets to on "continue" / "proceed to lobby". This
// is the CLIENT MIRROR of the server's reset (table_continue): status → waiting,
// each seat IDLE (human) / READY (bot) with an empty hand, and every volatile
// round field cleared. Used to transition the win screen to the lobby
// OPTIMISTICALLY (before the meta round-trip); the authoritative reset that
// follows must match this on the public fields or the user sees a snap
// (e2e/meta.test.ts holds the two together). Returns a NEW board (no mutation).
// Phase 6b moves optimistic boards into the kernel (client_optimistic_apply).
export const resetToLobby = (view: TableView): TableView => ({
    ...view,
    status: GAME_STATUS.WAITING,
    seats: view.seats.map((s) => ({ ...s, status: s.isAi ? PLAYER_STATUS.READY : PLAYER_STATUS.IDLE, handCount: 0, awaitingAttack: false })),
    myHand: [],
    deckCount: 0,
    discardPileLength: 0,
    hasFlipped: false,
    flipped: NO_CARD,
    powerSuit: 0,
    firstAttacker: 0,
    defender: 0,
    fool: -1,
    battles: [],
    elimination: [],
    hasGoodTimestamp: false,
    goodMask: 0,
    // Keep the version: the authoritative reset broadcasts at a HIGHER
    // version, so the animation feed's reorder gate still accepts it.
});
const cardComp = sameCard;

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
// for broadcasts, applyOverlayEntries for resync), so appending stale leftover
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

// The debounced hand-rearrange flush (DragContext.scheduleCardRearrangeUpdate ->
// ServerContext.rearrangeHand) applies `cardIndices` to the CURRENT hand as
// `indices.map(i => hand[i])`. Those indices were computed against an EARLIER
// hand snapshot, so by flush time the hand may have shrunk (a card played,
// drawn, or picked up) — a now-out-of-range index yields `hand[i] === undefined`,
// minting a hole that crashes the render on `card.suit` (the same "e.suit" prod
// crash reorderHand guards on the swap side). Only apply when the indices are a
// true permutation of the current hand — same contract the server's
// handleRearrangeHand enforces (in range, unique, full-length); otherwise the
// caller abandons the stale reorder and keeps the authoritative order.
export const isHandPermutation = (cardIndices: number[], handLength: number): boolean =>
    Array.isArray(cardIndices)
    && cardIndices.length === handLength
    && new Set(cardIndices).size === handLength
    && cardIndices.every((i) => Number.isInteger(i) && i >= 0 && i < handLength);

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

// ---- Optimistic overlay (resync preservation) ------------------------------
export interface OverlayEntry { card: Card; target?: Card | null }

// Re-apply the local player's unconfirmed optimistic cards onto an
// authoritatively-loaded board, so a reconnect resync doesn't momentarily drop a
// just-played card. Idempotent; returns the board itself when there is nothing
// to apply (a spectator's, or no pending card).
export const applyOverlayEntries = (v: TableView, entries: OverlayEntry[]): TableView => {
    if (!entries || entries.length === 0 || v.mySeat < 0) return v;
    const battles = v.battles.map((b) => ({ ...b }));
    let hand: readonly Card[] = v.myHand;
    for (const e of entries) {
        if (e.target) {
            const battle = battles.find((b) => cardComp(b.attack, e.target!) && !covered(b));
            if (battle) battle.defense = e.card;
        } else {
            const present = battles.some((b) => cardComp(b.attack, e.card) || (covered(b) && cardComp(b.defense, e.card)));
            if (!present) battles.push({ attack: e.card, defense: NO_CARD });
        }
        hand = hand.filter((c) => !cardComp(c, e.card));
    }
    return { ...v, battles, myHand: hand };
};
