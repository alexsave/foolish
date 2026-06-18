// Pure client-side reconciliation logic, extracted from ServerContext /
// AnimationContext so the SAME deployed code can be unit-tested directly (no React
// needed). The components import from here; the e2e suite imports from here. There
// is no second copy.

import { Card, PersonalGame, Battle } from '@shared/types.ts';

export const cardKey = (c: Card): string => `${c.suit}-${c.value}`;
export const cardComp = (a: Card, b: Card): boolean => a.suit === b.suit && a.value === b.value;

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
