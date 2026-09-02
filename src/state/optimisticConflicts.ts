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

import { Card } from '@api/core/types.ts';
import { getCardKey } from '../utils/animationUtils';
import { animResolveUnconfirmed, animEventTypeCode } from '@sdk/ts/wasm/bots.ts';

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
    // The DECISION lives in the C animation core (c/src/anim_plan.h
    // anim_resolve_unconfirmed_attack_covers), reached through the wasm bridge —
    // the whole revert-vs-keep-vs-clear choreography, hardened by the web
    // glitch-fixing, in one implementation the phone and a Steam client share.
    // This wrapper marshals the fields C needs and maps the returned indices back
    // to Cards. Asserted natively (c/tests/anim_plan_test.c test_optimistic_revert)
    // and end-to-end through this delegation by e2e/optimistic_revert.test.ts.
    // The original TS body is preserved in src/state/__ts_reference.ts and proven
    // identical to this delegation by e2e/anim_core_parity.test.ts, until the
    // deferred TS deletion (docs/ANIMATION_CORE_C.md).
    if (myOptimisticAttackCovers.length === 0) return { revert: [], merge: [], clear: [] };

    const pending = myOptimisticAttackCovers.map((card) => ({
        card,
        isCover: myOptimisticCoverKeys ? myOptimisticCoverKeys.has(getCardKey(card)) : false,
    }));
    const bridgeEvents = events.map((evt) => ({ type: animEventTypeCode(evt.type), cards: evt.cards ?? [] }));

    // Defender scalars, exactly as the old inline capacity check read them:
    // an undefined defender yields a 0 hand size (defender = -1 for the C side).
    const defender = finalGameState?.defender !== undefined ? finalGameState.defender : -1;
    const defenderHand = finalGameState?.defender !== undefined
        ? (finalGameState.players?.[finalGameState.defender]?.hand_length ?? 0)
        : 0;
    const finalUncovered = finalGameState?.table_battles?.filter((b) => !b.defense).length ?? 0;

    const r = animResolveUnconfirmed(pending, serverTableCards, bridgeEvents,
        { defender, defenderHand, finalUncovered });
    return {
        revert: r.revert.map((i) => pending[i].card),
        merge: r.merge.map((i) => pending[i].card),
        clear: r.clear.map((i) => pending[i].card),
    };
}

// Re-export so callers can key by card without another import.
export { getCardKey };
