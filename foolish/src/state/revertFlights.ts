// revertFlights.ts - what a doomed prediction's return flight plays OVER, and WHEN.
//
// optimisticResolve.ts decides WHICH of my unanswered cards a push dooms and
// builds a revert event for each. This file answers the two questions left: the
// board those flights are drawn against while they travel, and where they sit in
// the queue relative to the push's own events.
//
// THE BOARD is the problem the whole file exists for. A revert flies a card from
// where it is DRAWN back to my hand, so for the length of that flight the screen
// must still show the world the card was drawn into - the push's own board has
// already moved on and would teleport the card the moment it committed. So the
// board here is the push's first board with the cards that are about to move
// taken off it: the reverting cards, the valid attacks the push will animate
// itself, or only one of those, depending on which way the reverts and the
// push's sweep are travelling. Every edit is the kernel's (clientBoards.ts);
// nothing here writes a board field.
//
// THE ORDER is the kernel's rule, not a preference (anim_plan.h
// anim_conflict_reversal, reached through anim_reversal_order because the web
// decides doom under the SERVER transport).
//
// Nothing here reads the optimistic state - the reverts arrive already built, so
// this is board math over a push and a list, callable from a test with two plain
// objects. That is why it is not part of optimisticResolve.ts.

import { lifted, returnedToHand, tableOf, turnedBoard } from './clientBoards';
import type { TableView, ViewCard as Card } from './view';
import type { ClientAnimationEvent } from './animationStep';
import { ANIM_CONFLICT_REVERT, animReversalOrder } from '@sdk/ts/wasm/bots.ts';

/** The push whose events the reverts are being staged against. */
interface IncomingPush {
    events: any[];
}

/**
 * The queue for a push that doomed some of my predictions: every revert flight
 * first, in the kernel's reversal order, then the push's own events.
 *
 * Each revert event is given the board it plays over, in place.
 */
export function revertsFirst(
    message: IncomingPush,
    revertEvents: ClientAnimationEvent[],
    passIsInvalid: boolean,
): ClientAnimationEvent[] {
    // If there are revert events, we need to keep invalid cards on table until revert animates

    // Give revert events a game state that includes optimistic cards on table
    // This prevents teleporting when server events update the state

    // Get server state from the first event with state
    const firstEventWithState = message.events.find((evt: any) => evt.game_state);
    const serverStateForRevert = firstEventWithState?.game_state;

    // Check if we have pickup reverts (hand → table)
    const hasPickupRevertsForState = revertEvents.some(rev => rev.to_location === 'table');

    // For pickup revert scenarios, reconstruct table state
    const pickupEvent = message.events.find((evt: any) => evt.type === 'pickup' || evt.type === 'cards_to_trash');
    const magicTransitionEvent = message.events.find((evt: any) => evt.type === 'magic_transition');

    let baseState: TableView | null;
    if (hasPickupRevertsForState) {
        // For pickup reverts, we need state with cards on table
        if (magicTransitionEvent?.game_state) {
            // Use magic_transition state (has cards on table before good)
            baseState = magicTransitionEvent.game_state;
        } else if (pickupEvent?.cards) {
            // Reconstruct state with cards on table (before pickup): the
            // cards back on the table as uncovered attacks
            baseState = serverStateForRevert ? tableOf(serverStateForRevert, pickupEvent.cards) : null;
        } else {
            baseState = serverStateForRevert;
        }
    } else {
        baseState = serverStateForRevert;
    }

    let stateWithOptimistic: TableView | null = baseState ?? null;

    // Check if we have pass reverts - they need original defender value
    const hasPassReverts = revertEvents.some(rev =>
        rev.to_location === 'hand' &&
        passIsInvalid // We detected an invalid pass earlier
    );

    if (hasPassReverts && stateWithOptimistic && serverStateForRevert) {
        // For pass reverts, use the SERVER's defender value (original before pass)
        stateWithOptimistic = turnedBoard(stateWithOptimistic, serverStateForRevert.firstAttacker, serverStateForRevert.defender);
    }

    if (stateWithOptimistic) {
        // IMPORTANT: Remove BOTH optimistic cards AND cards that will be animated
        // This prevents the "transform" issue where invalid card becomes valid card

        const revertCards: Card[] = revertEvents.flatMap(evt => [...(evt.cards ?? [])]);

        // For attack conflicts: the cards that will be animated (valid attacks)
        const serverAttackCards: Card[] = message.events
            .filter((evt: any) => evt.type === 'attack_pass' && evt.from_location === 'hand')
            .flatMap((evt: any) => evt.cards ?? []);

        // Check if we have pickup reverts (hand → table) vs attack reverts (table → hand)
        const hasPickupRevertsForClean = revertEvents.some(rev => rev.to_location === 'table');
        const hasAttackReverts = revertEvents.some(rev => rev.to_location === 'hand');
        const hasPickupEventForClean = message.events.some((evt: any) => evt.type === 'pickup' || evt.type === 'cards_to_trash');

        // The battles that leave the board, by a card either side of them.
        let liftedCards: Card[];
        if (hasPickupRevertsForClean) {
            // PICKUP REVERT SCENARIO (hand → table):
            // State should show reverted cards on table, but NOT server attack cards that will animate
            liftedCards = serverAttackCards;
        } else if (hasPickupEventForClean && hasAttackReverts) {
            // ATTACK REVERT + PICKUP SCENARIO: Only remove reverting cards, keep everything else
            // (Cards to be picked up need to stay on table for pickup animation)
            liftedCards = revertCards;
        } else {
            // ATTACK CONFLICT SCENARIO: Remove both reverting AND valid attacks that will animate
            liftedCards = [...revertCards, ...serverAttackCards];
        }
        stateWithOptimistic = lifted(stateWithOptimistic, liftedCards);

        // For pass reverts (table → hand), add cards back to player's hand
        if (hasPassReverts && stateWithOptimistic) {
            const passRevertCards = revertEvents
                .filter(rev => rev.to_location === 'hand')
                .flatMap(rev => rev.cards || []);

            if (passRevertCards.length > 0) {
                // Each card not already in hand goes back into it
                stateWithOptimistic = returnedToHand(stateWithOptimistic, passRevertCards);
            }
        }
    }

    revertEvents.forEach((revertEvent) => {
        revertEvent.game_state = (stateWithOptimistic ?? undefined) as TableView | undefined;
    });

    // THE BOARD REVERSES WHAT IT MUST BEFORE IT PLAYS ANYTHING ELSE, and in
    // reverse group order: the cards travel back the way they came, last
    // motion first, and only then does the arriving stream animate forward.
    //
    // That is the kernel's rule (c/src/anim_plan.h anim_conflict_reversal,
    // reached here through anim_reversal_order because the web decides doom
    // under the SERVER transport), and it REPLACES what it met rather than
    // being reconciled with it - the standing rule of this migration. What
    // it met was four branches choosing where a return flight went relative
    // to the stream's own events: before a magic transition, before the
    // first attack from hand "for parallel visual effect", before a pickup,
    // or first. The parallel-effect one was the workaround for not having a
    // reversal step at all: the revert and the valid attack never did
    // overlap, they were two flights the author hoped would read as one.
    //
    // Each revert event is one group, because each is one parallel step the
    // prediction flew, and each is already a REVERT: resolveOptimisticConflicts
    // builds an event only for the cards the kernel's verdict doomed.
    const reversal = animReversalOrder(
        revertEvents.map(() => ANIM_CONFLICT_REVERT),
        revertEvents.map(() => 1));
    return [...reversal.flat().map((i) => revertEvents[i]), ...message.events];
}
