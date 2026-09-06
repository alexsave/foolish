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
import {
    animConflictVerdicts, animEventTypeCode, ANIM_DEST,
    AnimConflictInputs, AnimConflictMotion, AnimConflictVerdict,
} from '@sdk/ts/wasm/bots.ts';

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
    // anim_conflict_verdict), reached through resolveConflictMotions below - the
    // whole revert-vs-keep-vs-clear choreography, hardened by the web
    // glitch-fixing, in one implementation the phone and a Steam client share.
    // Asserted natively (c/tests/anim_plan_test.c test_optimistic_revert) and
    // end-to-end through this delegation by e2e/optimistic_revert.test.ts.
    //
    // This is the ATTACK/COVER shape of the question: every card landed on the
    // table, and the capacity rule measures the FINAL board's defender. It used
    // to reach C through a second wasm entry of its own (wasm_anim_resolve);
    // that door is gone, because two doors onto one rule is a new way for two
    // hosts to disagree.
    if (myOptimisticAttackCovers.length === 0) return { revert: [], merge: [], clear: [] };

    // Defender scalars, exactly as the old inline capacity check read them:
    // an undefined defender yields a 0 hand size.
    const defenderHand = finalGameState?.defender !== undefined
        ? (finalGameState.players?.[finalGameState.defender]?.hand_length ?? 0)
        : 0;
    const finalUncovered = finalGameState?.table_battles?.filter((b) => !b.defense).length ?? 0;

    const r = resolveConflictMotions(
        myOptimisticAttackCovers.map((card) => ({
            card,
            dest: ANIM_DEST.table,
            isCover: myOptimisticCoverKeys ? myOptimisticCoverKeys.has(getCardKey(card)) : false,
        })),
        {
            events: conflictEvents(events),
            openTable: serverTableCards.map((attack) => ({ attack, defense: null })),
            myHand: [],
            defenderHand,
            finalUncovered,
        });
    // `merge` is this caller's word for the rule's KEEP.
    return { revert: r.revert, merge: r.keep, clear: r.clear };
}

/** The app's animation events as the kernel reads them: a type code and the
 *  cards. Which of them SWEEP is the kernel's call, not this file's. */
export const conflictEvents = (events: AnimEvent[]): AnimConflictInputs['events'] =>
    events.map((e) => ({ type: animEventTypeCode(e.type), cards: e.cards ?? [] }));

/**
 * THE CONFLICT VERDICT for a set of motions, straight from the kernel
 * (anim_plan.h anim_conflict_facts + anim_conflict_verdict).
 *
 * resolveUnconfirmedAttackCovers above is this same rule bundled for one shape.
 * AnimationContext asks it about three more: a pass judged against the next
 * defender, my attacks after a pass moved the shield, and a pickup, whose cards
 * landed in my HAND rather than on the table. Those used to be inline
 * TypeScript that checked only capacity, so they had none of the rule's
 * precedence (CLEAR before the standing sets), its pool rule or its masked-back
 * rule - which is the flicker c/src/anim_plan.h opens by describing.
 *
 * The motions and inputs are the kernel's own shapes (AnimConflictMotion /
 * AnimConflictInputs). There is no second set of types here to keep in step
 * with them, and no sweep derivation either: the events go in whole and
 * anim_conflict_sweep decides which of them took the table away.
 *
 * `pendingAttacks` defaults to the non-cover motions, which is what every
 * caller means by it; pass a value only to override.
 */
export function resolveConflictMotions(
    motions: AnimConflictMotion[],
    inputs: Omit<AnimConflictInputs, 'pendingAttacks'> & { pendingAttacks?: number },
): { revert: Card[]; keep: Card[]; clear: Card[] } {
    const out: { revert: Card[]; keep: Card[]; clear: Card[] } = { revert: [], keep: [], clear: [] };
    if (motions.length === 0) return out;

    const verdicts: AnimConflictVerdict[] = animConflictVerdicts(motions, {
        ...inputs,
        pendingAttacks: inputs.pendingAttacks ?? motions.filter((m) => !m.isCover).length,
    });
    verdicts.forEach((v, i) => {
        const card = motions[i].card;
        if (card) out[v === 'keep' ? 'keep' : v].push(card);
    });
    return out;
}

// Re-export so callers can key by card without another import.
export { getCardKey };
