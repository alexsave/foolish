// The "my optimistically-played card is not (yet) where I put it on the
// authoritative board" decision, for AnimationContext's resolveOptimisticConflicts
// and the e2e suite alike (no second copy).
//
// When a versioned broadcast lands whose boards do NOT show one of the local
// player's still-pending optimistic cards, we must decide, per card, whether to:
//   - REVERT it (fly it back - it was genuinely never accepted),
//   - KEEP it (merge it into the incoming boards so it stays put until its own
//     confirming broadcast / the server's verdict on our action resolves it), or
//   - CLEAR it (the broadcast's own pickup or trash carries it off; drop its
//     tracking and let that event animate it).
//
// Getting this wrong is the "card jumps to the table, snaps back to my hand, then
// re-appears on the table" flicker players see when they play a card at almost the
// same moment as another player, or when a defender picks the card up immediately.
//
// THE DECISION IS THE KERNEL'S, read off the push's boards themselves
// (c/src/client_table.h client_conflict_verdicts over anim_plan.h
// anim_conflict_verdict): where the push's cards stand, the defender's hand that
// bounds my attacks, the uncovered attacks. This file names the question.
// Asserted natively (c/tests: test_client_conflict_verdicts, anim_plan_test.c
// test_optimistic_revert) and end-to-end by e2e/optimistic_revert.test.ts.

import * as V from '@sdk/ts/gen/view_layout.bots.ts';
import { getCardKey } from '../utils/animationUtils';
import { animEventTypeCode } from '@sdk/ts/wasm/bots.ts';
import { clientTable } from '@sdk/ts/table/client_table.ts';
import type { TableView, ViewCard as Card } from './view';

export interface AttackCoverResolution {
    /** Optimistic attack/cover cards to fly back to hand (genuinely never accepted). */
    revert: Card[];
    /** Optimistic attack/cover cards to keep and merge into the incoming states. */
    merge: Card[];
    /**
     * Optimistic attack/cover cards that WERE accepted onto the table and then swept
     * off it by this broadcast's own pickup/cards_to_trash - drop their optimistic
     * tracking WITHOUT a revert animation; the clear event itself animates them off
     * the table. Reverting these to hand is the "I put a card down and someone picked
     * it up, and it flew back to my hand" flicker.
     */
    clear: Card[];
}

interface AnimEvent { type?: string; cards?: readonly Card[] }

/** Where a pending motion put its card (anim_plan.h ANIM_DEST_*). */
export const CONFLICT_DEST = { table: V.ANIM_DEST_TABLE, hand: V.ANIM_DEST_MY_HAND } as const;

/** One pending motion: which card, where it went, and whether it was the defender's own cover. */
export interface ConflictMotion { card: Card; dest: number; isCover?: boolean }

/** The push a pending motion is judged against. */
export interface ConflictPush {
    /** The push's events this client has not already played. Which of them sweep is the kernel's call. */
    events: readonly AnimEvent[];
    /** The push's last board: where its cards stand, on the table and in my hand. */
    open: TableView;
    /** The push's final board. */
    final: TableView;
    /** The seat whose hand on the final board bounds my pending attacks; -1 for none. */
    defenderSeat: number;
    /** Count the uncovered attacks on the final board rather than the open one. */
    uncoveredOnFinal?: boolean;
    /** My pending attacks, when they are not simply the motions that are not covers. */
    pendingAttacks?: number;
}

/**
 * THE CONFLICT VERDICT for a set of motions, straight from the kernel. All four
 * shapes AnimationContext asks about come through here.
 */
export function resolveConflictMotions(motions: readonly ConflictMotion[], push: ConflictPush): { revert: Card[]; keep: Card[]; clear: Card[] } {
    const out: { revert: Card[]; keep: Card[]; clear: Card[] } = { revert: [], keep: [], clear: [] };
    if (motions.length === 0) return out;
    const verdicts = clientTable().conflictVerdicts(push.open, push.final, {
        defenderSeat: push.defenderSeat,
        uncoveredOnFinal: push.uncoveredOnFinal ?? false,
        pendingAttacks: push.pendingAttacks ?? -1,
        events: push.events.map((e) => ({ type: animEventTypeCode(e.type), masked: false, cards: e.cards ?? [] })),
        motions: motions.map((m) => ({ card: m.card, dest: m.dest, isCover: !!m.isCover })),
    });
    verdicts.forEach((v, i) => {
        out[v === V.ANIM_CONFLICT_KEEP ? 'keep' : v === V.ANIM_CONFLICT_CLEAR ? 'clear' : 'revert'].push(motions[i].card);
    });
    return out;
}

/**
 * My pending attack and cover cards the push's last board does not show.
 *
 * @param myOptimisticAttackCovers the local player's pending optimistic attack/cover cards
 * @param open                     the push's last board (the last event's game_state)
 * @param events                   the push's animation events
 * @param final                    the push's final board as this viewer sees it
 * @param myOptimisticCoverKeys    getCardKey()s of the pending cards that are COVERS -
 *                                 the defender-capacity rule applies only to attacks
 */
export function resolveUnconfirmedAttackCovers(
    myOptimisticAttackCovers: Card[],
    open: TableView,
    events: readonly AnimEvent[],
    final: TableView,
    myOptimisticCoverKeys?: Set<string>,
): AttackCoverResolution {
    if (myOptimisticAttackCovers.length === 0) return { revert: [], merge: [], clear: [] };
    // Every card landed on the table, and the capacity is the final board's
    // defender's, against the final board's uncovered attacks.
    const r = resolveConflictMotions(
        myOptimisticAttackCovers.map((card) => ({
            card,
            dest: CONFLICT_DEST.table,
            isCover: myOptimisticCoverKeys ? myOptimisticCoverKeys.has(getCardKey(card)) : false,
        })),
        { events, open, final, defenderSeat: final.defender, uncoveredOnFinal: true });
    // `merge` is this caller's word for the rule's KEEP.
    return { revert: r.revert, merge: r.keep, clear: r.clear };
}

// Re-export so callers can key by card without another import.
export { getCardKey };
