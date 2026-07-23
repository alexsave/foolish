// PARITY REFERENCE ONLY — dead code except under test.
//
// These are the ORIGINAL TypeScript implementations of the animation policies
// that now live in the C animation core (c/src/anim_plan.h) and run in
// production through the wasm bridge (sdk/ts/wasm/bots.ts). They are preserved
// here, verbatim, so e2e/anim_core_parity.test.ts can drive BOTH the old TS and
// the new C over the same live-shaped + hostile inputs and assert identical
// results BEFORE the TS is ever deleted.
//
// Nothing in the app imports this module. It exists only for the parity soak.
// Once parity has held in CI, these bodies (and this file) are deleted; see
// docs/ANIMATION_CORE_C.md "Deferred TS deletion" for the list.
//
// DO NOT "fix" or refactor these to match the C — that would defeat the point.
// They must stay byte-for-byte the logic the React app shipped and hardened.

import { Card } from '@api/core/types.ts';
import { getCardKey, cardsIntersection } from '../utils/animationUtils';

// ---- optimisticAnimation.staleOptimisticKeysOnTable (original) -------------
interface AnimEvent { type?: string; cards?: Card[]; [k: string]: unknown }

export function staleOptimisticKeysOnTableTsReference(
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

// ---- optimisticConflicts.resolveUnconfirmedAttackCovers (original) ---------
export interface AttackCoverResolution { revert: Card[]; merge: Card[]; clear: Card[] }
interface GameStateLike { defender?: number; players?: { hand_length?: number }[]; table_battles?: { defense?: unknown }[] }

export function resolveUnconfirmedAttackCoversTsReference(
    myOptimisticAttackCovers: Card[],
    serverTableCards: Card[],
    events: AnimEvent[],
    finalGameState: GameStateLike | null | undefined,
    myOptimisticCoverKeys?: Set<string>,
): AttackCoverResolution {
    const myOptimisticCardsAccepted = cardsIntersection(myOptimisticAttackCovers, serverTableCards);
    if (myOptimisticAttackCovers.length === 0 || myOptimisticCardsAccepted.length > 0) {
        return { revert: [], merge: [], clear: [] };
    }

    const hasTableClearEvent = events.some((evt) => evt.type === 'pickup' || evt.type === 'cards_to_trash');
    if (hasTableClearEvent) {
        const sweptKeys = new Set<string>();
        for (const evt of events) {
            if (evt.type === 'pickup' || evt.type === 'cards_to_trash') {
                for (const c of evt.cards ?? []) sweptKeys.add(getCardKey(c));
            }
        }
        const revert: Card[] = [];
        const clear: Card[] = [];
        for (const c of myOptimisticAttackCovers) (sweptKeys.has(getCardKey(c)) ? clear : revert).push(c);
        return { revert, merge: [], clear };
    }

    const pendingAttacks = myOptimisticCoverKeys
        ? myOptimisticAttackCovers.filter((c) => !myOptimisticCoverKeys.has(getCardKey(c)))
        : myOptimisticAttackCovers;
    const defenderHandSize = finalGameState?.defender !== undefined
        ? (finalGameState.players?.[finalGameState.defender]?.hand_length ?? 0)
        : 0;
    const finalUncoveredAttacks = finalGameState?.table_battles?.filter((b) => !b.defense).length ?? 0;
    const totalAttacks = finalUncoveredAttacks + pendingAttacks.length;
    if (pendingAttacks.length > 0 && totalAttacks > defenderHandSize) {
        const pendingCovers = myOptimisticAttackCovers.filter(
            (c) => myOptimisticCoverKeys?.has(getCardKey(c)),
        );
        return { revert: [...pendingAttacks], merge: pendingCovers, clear: [] };
    }

    return { revert: [], merge: [...myOptimisticAttackCovers], clear: [] };
}

// ---- clientReconcile.shouldDropStaleSequence (original) --------------------
export const shouldDropStaleSequenceTsReference = (
    lastAppliedVersion: number | null, incomingVersion: number | null,
): boolean => {
    if (incomingVersion === null) return false;
    return lastAppliedVersion !== null && incomingVersion <= lastAppliedVersion;
};

// ---- plan building (a faithful TS mirror of anim_plan.c anim_build_plan) ----
// There was no standalone TS "original" for this — the web re-derived the same
// choreography inside AnimationContext's setState/timeout machinery and iOS did
// it in Swift (preCounts + preHide). This mirror encodes the SAME algorithm so
// the parity test guards the C build_plan and its wasm marshalling against an
// independent implementation.

export const REF_ANIM_TIME_MS = 500;
export const REF_ANIM_GAP_MS = 25;
// ANIM_EVT_* (anim_plan.h)
const REF_DEAL = 1, REF_REFILL = 9, REF_DISCARD = 7, REF_CARDS_TO_TRASH = 10,
      REF_PICKUP = 6, REF_ATTACK_PASS = 4, REF_COVER = 5, REF_DEFENDER_MOVE = 3;
// ANIM_LOC_* (anim_plan.h)
const REF_LOC_DECK = 0, REF_LOC_HAND = 1, REF_LOC_TABLE = 2, REF_LOC_FLIPPED = 4;

export interface RefPlanEvent {
    type: number; seat: number | null; from: number; to: number;
    mask: boolean; cards: { suit: number; value: number }[];
}
export interface RefPlanStep {
    type: number; seat: number; from: number; to: number; nCards: number;
    durationMs: number; startMs: number; deck: number; discard: number;
    inFlightFromDeck: number; inFlightToFlipped: number; hand: number[];
}
export interface RefPlan {
    nSteps: number; nPlayers: number;
    pre: { deck: number; discard: number; hand: number[] };
    totalMs: number; veilIds: number[]; steps: RefPlanStep[];
}

const refCardId = (c: { suit: number; value: number }): number =>
    (c.suit < 0 || c.value < 1 || c.value > 13) ? -1 : c.suit * 13 + (c.value - 1);

function refForward(ev: RefPlanEvent, c: { deck: number; discard: number; hand: number[] }, np: number): void {
    const n = ev.cards.length, s = ev.seat;
    switch (ev.type) {
        case REF_DEAL: case REF_REFILL:
            c.deck -= n; if (s !== null && s >= 0 && s < np) c.hand[s] += n; break;
        case REF_DISCARD: case REF_CARDS_TO_TRASH: c.discard += n; break;
        case REF_PICKUP: if (s !== null && s >= 0 && s < np) c.hand[s] += n; break;
        case REF_ATTACK_PASS: case REF_COVER: case REF_DEFENDER_MOVE:
            if (s !== null && s >= 0 && s < np) c.hand[s] -= n; break;
        default: break;
    }
}
function refUndo(ev: RefPlanEvent, c: { deck: number; discard: number; hand: number[] }, np: number): void {
    const n = ev.cards.length, s = ev.seat;
    switch (ev.type) {
        case REF_DEAL: case REF_REFILL:
            c.deck += n; if (s !== null && s >= 0 && s < np) c.hand[s] -= n; break;
        case REF_DISCARD: case REF_CARDS_TO_TRASH: c.discard -= n; break;
        case REF_PICKUP: if (s !== null && s >= 0 && s < np) c.hand[s] -= n; break;
        case REF_ATTACK_PASS: case REF_COVER: case REF_DEFENDER_MOVE:
            if (s !== null && s >= 0 && s < np) c.hand[s] += n; break;
        default: break;
    }
}

export function buildAnimPlanTsReference(
    events: RefPlanEvent[], nPlayers: number,
    finalDeck: number, finalDiscard: number, finalHand: number[],
): RefPlan {
    const cur = { deck: finalDeck, discard: finalDiscard, hand: finalHand.slice(0, nPlayers) };
    for (let i = events.length - 1; i >= 0; i--) refUndo(events[i], cur, nPlayers);
    const pre = { deck: cur.deck, discard: cur.discard, hand: cur.hand.slice() };

    const stride = REF_ANIM_TIME_MS + REF_ANIM_GAP_MS;
    const seen = new Set<number>();
    const veilIds: number[] = [];
    const steps: RefPlanStep[] = [];
    for (let i = 0; i < events.length; i++) {
        const ev = events[i];
        refForward(ev, cur, nPlayers);
        const st: RefPlanStep = {
            type: ev.type, seat: ev.seat === null ? -1 : ev.seat, from: ev.from, to: ev.to,
            nCards: ev.cards.length, durationMs: REF_ANIM_TIME_MS, startMs: i * stride,
            deck: cur.deck, discard: cur.discard,
            inFlightFromDeck: 0, inFlightToFlipped: 0, hand: cur.hand.slice(),
        };
        if (ev.from === REF_LOC_DECK && ev.cards.length > 0) {
            st.inFlightFromDeck = ev.cards.length;
            st.inFlightToFlipped = ev.to === REF_LOC_FLIPPED ? ev.cards.length : 0;
        }
        steps.push(st);
        if ((ev.to === REF_LOC_HAND || ev.to === REF_LOC_TABLE) && !ev.mask) {
            for (const c of ev.cards) {
                const id = refCardId(c);
                if (id < 0 || seen.has(id)) continue;
                seen.add(id); veilIds.push(id);
            }
        }
    }
    const totalMs = events.length > 0
        ? steps[events.length - 1].startMs + steps[events.length - 1].durationMs : 0;
    return { nSteps: events.length, nPlayers, pre, totalMs, veilIds, steps };
}
