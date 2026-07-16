// Pure client-side reconciliation logic, extracted from ServerContext /
// AnimationContext so the SAME deployed code can be unit-tested directly (no React
// needed). The components import from here; the e2e suite imports from here. There
// is no second copy.

import { Card, PersonalGame, Battle, GAME_STATUS, PLAYER_STATUS } from '@shared/core/types.ts';

export const cardKey = (c: Card): string => `${c.suit}-${c.value}`;

// The lobby a finished game resets to on "continue" / "proceed to lobby". This
// is the CLIENT MIRROR of the server's handleContinue reset
// (_shared/meta_actions.ts): status → waiting, each player IDLE (human) / READY
// (bot) with an empty hand, and every volatile round field cleared. Used to
// transition the win screen to the lobby OPTIMISTICALLY (before the meta
// round-trip); the authoritative reset that follows must match this byte-for-byte
// on the public fields or the user sees a snap. Kept here, pure and unit-tested,
// so it can't silently drift from the server. Returns a NEW game (no mutation).
export const resetToLobby = (game: PersonalGame): PersonalGame => {
    const resetStatus = (is_ai: boolean) => is_ai ? PLAYER_STATUS.READY : PLAYER_STATUS.IDLE;
    return {
        ...game,
        status: GAME_STATUS.WAITING,
        players: game.players.map(p => ({ ...p, status: resetStatus(p.is_ai), hand_length: 0 })),
        self: game.self
            ? { ...game.self, status: resetStatus(game.self.is_ai), hand: [], hand_length: 0, awaiting_attack: false }
            : game.self,
        deck_length: 0,
        discard_pile_length: 0,
        flipped: null,
        power_suit: 0,
        first_attacker: 0,
        defender: 0,
        table_battles: [],
        elimination_order: [],
        good_timestamp: null,
        good_players: [],
        // Keep game.version: the authoritative reset broadcasts at a HIGHER
        // version, so the animation feed's reorder gate still accepts it.
    };
};
const cardComp = (a: Card, b: Card): boolean => a.suit === b.suit && a.value === b.value;

// ---- Live broadcast ordering gate -----------------------------------------
// Broadcasts are fired un-awaited over per-call channels, so under realtime
// latency they can arrive out of order. Each carries the committed games.version;
// drop any whose version is at or below the newest already applied (it's strictly
// superseded — each sequence carries the full resulting state). Replay sequences
// have no version and are never gated.
export const shouldDropStaleSequence = (lastAppliedVersion: number | null, incomingVersion: number | null): boolean => {
    if (incomingVersion === null) return false;
    return lastAppliedVersion !== null && incomingVersion <= lastAppliedVersion;
};

// ---- Table reconciliation --------------------------------------------------
// Trust the server's table outright. Ordering is handled by the version gate and
// the local player's unconfirmed cards are injected upstream (optimistic resolver
// for broadcasts, applyOverlayEntries for resync), so appending stale leftover
// battles is unnecessary and would re-introduce a previous bout's cards when an
// intermediate clear is skipped.
export const mergeTableBattles = (existingBattles: Battle[] | undefined, incomingBattles: Battle[] | undefined): Battle[] => {
    return incomingBattles ?? existingBattles ?? [];
};

// ---- Hand reconciliation ---------------------------------------------------
// Legacy order-preserving merge (kept for game.self.hand). Appends new cards at
// the end; superseded for the RENDERED hand by reconcileHandMemory + displayedHand.
export const mergeHandOrder = (oldHand: Card[], newHand: Card[]): Card[] => {
    if (!oldHand || !newHand) return newHand || [];
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
export const reorderHand = (order: Card[], fromIndex: number, toIndex: number): Card[] => {
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
export const reconcileHandMemory = (memory: Card[], authHand: Card[]): Card[] => {
    const seen = new Set<string>();
    const dedupMem = (memory || []).filter((c) => { const k = cardKey(c); if (seen.has(k)) return false; seen.add(k); return true; });
    const additions = (authHand || []).filter((c) => !seen.has(cardKey(c)));
    return [...dedupMem, ...additions];
};

// The rendered hand: the authoritative hand, deduplicated and ordered by the
// memory. A card not in the authoritative hand (played, or on the table) is never
// shown; duplicates are impossible by construction.
export const displayedHand = (memory: Card[], authHand: Card[]): Card[] => {
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
// authoritatively-loaded game, so a reconnect resync doesn't momentarily drop a
// just-played card. Idempotent.
export const applyOverlayEntries = (g: PersonalGame, entries: OverlayEntry[]): void => {
    if (!entries || entries.length === 0 || !g.self) return;
    for (const e of entries) {
        if (e.target) {
            const battle = g.table_battles.find((b) => cardComp(b.attack, e.target!) && b.defense === null);
            if (battle) battle.defense = e.card;
        } else {
            const present = g.table_battles.some((b) =>
                cardComp(b.attack, e.card) || (b.defense !== null && cardComp(b.defense, e.card)));
            if (!present) g.table_battles.push({ attack: e.card, defense: null });
        }
        if (g.self.hand) g.self.hand = g.self.hand.filter((c) => !cardComp(c, e.card));
    }
};
