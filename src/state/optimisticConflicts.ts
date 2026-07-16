// Pure decision logic for the "my optimistically-played attack/cover card is not
// (yet) on the authoritative table" case, extracted from AnimationContext's
// resolveOptimisticConflicts so the SAME deployed logic is unit-testable (the
// component imports from here; the e2e suite imports from here — no second copy).
//
// When a versioned broadcast lands whose server table does NOT include one of the
// local player's still-pending optimistic attack/cover cards, we must decide, per
// card, whether to:
//   - REVERT it (fly it back to hand — it was genuinely never accepted), or
//   - KEEP it (merge it into the incoming states so it stays put until its own
//     confirming broadcast / the server's verdict on our action resolves it).
//
// Getting this wrong is the "card jumps to the table, snaps back to my hand, then
// re-appears on the table" flicker players see when they play a card at almost the
// same moment as another player, or when a defender picks the card up immediately.

import { Card } from '@shared/core/types.ts';
import { getCardKey, cardsIntersection } from '../utils/animationUtils';

export interface AttackCoverResolution {
    /** Optimistic attack/cover cards to fly back to hand (genuinely never accepted). */
    revert: Card[];
    /** Optimistic attack/cover cards to keep and merge into the incoming states. */
    merge: Card[];
    /**
     * Optimistic attack/cover cards that WERE accepted onto the table and then swept
     * off it by this broadcast's own pickup/cards_to_trash — drop their optimistic
     * tracking WITHOUT a revert animation; the clear event itself animates them off
     * the table. Reverting these to hand is the "I put a card down and someone picked
     * it up, and it flew back to my hand" flicker.
     */
    clear: Card[];
}

interface AnimEvent { type?: string; cards?: Card[] }
interface GameStateLike { defender?: number; players?: { hand_length?: number }[]; table_battles?: { defense?: unknown }[] }

/**
 * @param myOptimisticAttackCovers the local player's pending optimistic attack/cover cards
 * @param serverTableCards         the authoritative table cards this broadcast shows
 * @param events                   the broadcast's animation events
 * @param finalGameState           the broadcast's final personalized game state (message.game || serverState)
 * @param myOptimisticCoverKeys    getCardKey()s of the pending cards that are COVERS —
 *                                 the defender-capacity rule applies only to attacks
 *                                 (a cover is the defender's own play and has no
 *                                 capacity rule in the kernel; counting covers here
 *                                 used to false-revert legal in-flight covers)
 */
export function resolveUnconfirmedAttackCovers(
    myOptimisticAttackCovers: Card[],
    serverTableCards: Card[],
    events: AnimEvent[],
    finalGameState: GameStateLike | null | undefined,
    myOptimisticCoverKeys?: Set<string>,
): AttackCoverResolution {
    // Server already shows (some of) our cards on the table → accepted; this branch
    // does not apply and the cards are handled by the per-event dedup upstream.
    const myOptimisticCardsAccepted = cardsIntersection(myOptimisticAttackCovers, serverTableCards);
    if (myOptimisticAttackCovers.length === 0 || myOptimisticCardsAccepted.length > 0) {
        return { revert: [], merge: [], clear: [] };
    }

    // SPECIAL CASE: the server sent a pickup/cards_to_trash — the table is cleared.
    const hasTableClearEvent = events.some((evt) => evt.type === 'pickup' || evt.type === 'cards_to_trash');
    if (hasTableClearEvent) {
        // A card named by the clear event WAS on the table (accepted) and is now being
        // carried off it — don't revert it to hand; just drop its optimistic tracking.
        // A card NOT named by the clear event never reached the table (genuinely too
        // slow) → revert it.
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

    // Capacity check: can the defender still take all these attacks? A genuinely
    // illegal attack is also rejected by the server on our own action's response
    // (which reverts it there); this only reverts a hair sooner. Crucially it never
    // false-reverts a LEGAL in-flight attack: the server accepts our attack only if
    // uncovered_after ≤ defenderHand, and uncovered_after = finalUncoveredAttacks + 1,
    // so a legal attack keeps totalAttacks ≤ defenderHandSize here.
    //
    // The rule is an ATTACK rule (c/src/game.c handle_attack
    // DEFENDER_CAPACITY): pending COVERS are the defender's own play and are
    // excluded — they merge unless the sweep branch above already cleared them.
    const pendingAttacks = myOptimisticCoverKeys
        ? myOptimisticAttackCovers.filter((c) => !myOptimisticCoverKeys.has(getCardKey(c)))
        : myOptimisticAttackCovers;
    const defenderHandSize = finalGameState?.defender !== undefined
        ? (finalGameState.players?.[finalGameState.defender]?.hand_length ?? 0)
        : 0;
    const finalUncoveredAttacks = finalGameState?.table_battles?.filter((b) => !b.defense).length ?? 0;
    const totalAttacks = finalUncoveredAttacks + pendingAttacks.length;
    if (pendingAttacks.length > 0 && totalAttacks > defenderHandSize) {
        // Attack invalidated by earlier attack → revert (covers stay).
        const pendingCovers = myOptimisticAttackCovers.filter(
            (c) => myOptimisticCoverKeys?.has(getCardKey(c)),
        );
        return { revert: [...pendingAttacks], merge: pendingCovers, clear: [] };
    }

    // Defender can hold them → keep and merge into the incoming states.
    return { revert: [], merge: [...myOptimisticAttackCovers], clear: [] };
}

// Re-export so callers can key by card without another import.
export { getCardKey };
