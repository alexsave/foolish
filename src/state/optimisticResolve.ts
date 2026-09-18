// optimisticResolve.ts - MY MOVES THE SERVER HAS NOT ANSWERED, and what an
// arriving push does to them.
//
// A screen shows a card on the table the moment its owner taps it, long before
// any server says so. Until the answer comes the client owes three things about
// that card: WHERE it is drawn, WHETHER it is still pending, and - when a push
// lands that does not show it - whether it flies home, stays put, or is simply
// carried off by the push's own sweep. This file owns all three, as one bag of
// state (OptimisticState) and the routines that read and settle it.
//
// The VERDICT itself is not here and never was: optimisticConflicts.ts asks the
// kernel (c/src/client_table.h client_conflict_verdicts over anim_plan.h
// anim_conflict_verdict) which of my pending cards a push dooms. That module is
// the decision; this one is the orchestration around it - which cards to ask
// about, against which of the push's boards, and what to do with each answer:
// queue a revert flight, drop the tracking silently, or keep the card on every
// board the push carries. Keeping the two apart is deliberate. The decision is
// one question with one kernel answer and is worth reading on its own; the
// orchestration is five branches of client bookkeeping.
//
// Nothing here is React. The state is passed in, so the whole file is callable
// from a test with two Maps, a Set, a cell and a message - no provider to mount.
//
// Asserted end-to-end through the real provider, frame by frame, by
// e2e/ui_animation_trace.test.ts; the verdict underneath by
// e2e/optimistic_revert.test.ts and c/tests.

import { optimisticBoard, keepPending } from './clientBoards';
import { covered, tableCards, type TableView, type ViewCard as Card } from './view';
import { cardsIntersection, getCardKey } from '../utils/animationUtils';
import { resolveUnconfirmedAttackCovers, resolveConflictMotions, CONFLICT_DEST } from './optimisticConflicts';
import { staleOptimisticKeysOnTable } from './optimisticAnimation';
import { animEventKey } from '@sdk/ts/wasm/bots.ts';
import type { ClientAnimationEvent } from './animationStep';

// The board's own seat, if the board has one: the seat an event of mine is keyed by.
export const seatOf = (v: TableView | null | undefined): number | undefined =>
    v && v.mySeat >= 0 ? v.mySeat : undefined;

// A MOVE OF MINE THE SERVER HAS NOT CONFIRMED. The map of these is keyed by the
// kernel's own dedup key (c/src/anim_plan.h anim_event_key): two events collide
// iff they name the same (type, card, from, to, seat), and the seat stands in
// for the player id because a plan is per viewer. The record carries the fields
// back, so nothing here ever takes a key apart - which is what the key it
// replaced existed for. That was a JSON.stringify of these same five fields,
// JSON.parse'd back out in five places, i.e. a byte layout TypeScript knew.
export interface PendingMotion {
    type: string;
    card: Card;
    from: string;
    to: string;
    /** The acting seat, or undefined for a board with no seat of its own. */
    seat?: number;
    /** When it was predicted, for the sweep that drops motions nobody answered. */
    at: number;
}

/** Where a pending card is DRAWN while it waits, so a revert knows where to fly it home from. */
export interface OptimisticPosition {
    location: string;
    seat?: number;
    target_card?: Card;
    battle_index?: number;
}

/** MY PASS THE SERVER HAS NOT CONFIRMED, as the action wire I sent it (see `pass`). */
export interface PendingPass {
    wire: Uint8Array;
    cards: readonly Card[];
    seat?: number;
}

/**
 * Everything the client knows about its own unanswered moves.
 *
 * The three collections are mutated in place and never replaced, so a caller
 * holding React refs passes each ref's `.current` once. The pass is different:
 * it is REPLACED (a new pass, or null when it resolves), so it is passed as the
 * cell that holds it - which is exactly a ref's shape, and the reason this field
 * is not simply `PendingPass | null`.
 */
export interface OptimisticState {
    /** The kernel's dedup key -> what I predicted (see PendingMotion). */
    motions: Map<number, PendingMotion>;
    /** getCardKey()s of cards a revert flight is already carrying home. */
    reverting: Set<string>;
    /** getCardKey() -> where that card is drawn while it waits. */
    positions: Map<string, OptimisticPosition>;
    /** The cell holding my unconfirmed pass, if I have one. */
    pass: { current: PendingPass | null };
}

/** Remember a move of mine as pending, under the kernel's key for it. */
export const rememberPending = (
    s: OptimisticState, type: string, card: Card, from: string, to: string, seat: number | undefined, at: number,
): void => {
    s.motions.set(animEventKey(type, card, from, to, seat), { type, card, from, to, seat, at });
};

/** Drop predictions nobody ever answered: older than `maxAge` at `now`. */
export const sweepStaleMotions = (s: OptimisticState, now: number, maxAge: number): void => {
    const toDelete: number[] = [];
    s.motions.forEach((motion, key) => {
        if (now - motion.at > maxAge) {
            toDelete.push(key);
        }
    });
    toDelete.forEach((key) => {
        s.motions.delete(key);
    });
};

/**
 * My live optimistic table cards, for the REST load path: a reconnect resync
 * re-applies them instead of momentarily wiping them (the "vanish then reappear"
 * glitch). Derived on demand from the live position tracking, so it is always
 * current.
 */
export const pendingOverlayCards = (s: OptimisticState): { card: Card; target?: Card | null }[] => {
    const out: { card: Card; target?: Card | null }[] = [];
    s.positions.forEach((pos, cardKey) => {
        const [suit, value] = cardKey.split('-').map(Number);
        if (Number.isFinite(suit) && Number.isFinite(value)) {
            out.push({ card: { suit, value }, target: pos.target_card ?? null });
        }
    });
    return out;
};

/**
 * The pass is pending exactly while the optimistic map still holds one of its
 * cards. Every path that resolves a prediction already releases it there - the
 * confirming broadcast's dedup partition, the version gate, the conflict
 * reverts, a refusal, the sweep - so the pass needs no clearing discipline of
 * its own, which is the discipline the cached seats kept getting wrong.
 */
export const passStillPending = (s: OptimisticState): boolean => {
    const p = s.pass.current;
    if (!p) return false;
    const live = p.cards.some((c) => s.motions.has(animEventKey('attack_pass', c, 'hand', 'table', p.seat)));
    if (!live) s.pass.current = null;
    return live;
};

/** `board` with my still-unconfirmed pass standing on it, asked of the kernel;
 *  null when no pass is pending or the kernel refuses to change the board. */
export const passedBoard = (s: OptimisticState, board: TableView | null | undefined): TableView | null =>
    board && passStillPending(s) ? optimisticBoard(board, s.pass.current!.wire) : null;

/** A push as this file reads it. `events` and `game` are mutated in place when
 *  pending cards are kept on them, which is what makes the push carry them. */
interface IncomingPush {
    events: any[];
    game?: TableView;
}

/**
 * Release the predictions this push's AUTHORITATIVE board already shows on the
 * table but whose own confirming broadcast the version gate dropped.
 *
 * Cards this push DOES name are deliberately left alone: the dedup partition
 * below matches them and skips re-animating them, so releasing them here first
 * would make their own confirming event look un-optimistic and animate a SECOND
 * time (the double-play bug). The decision is the kernel's; optimisticAnimation.ts
 * is the marshal. Call this only on the pristine server board - before
 * resolveOptimisticConflicts has kept anything on it.
 */
export function releaseConfirmedOnTable(s: OptimisticState, message: IncomingPush): void {
    if (!message.game?.battles || s.motions.size <= 0) return;
    const onTable: Card[] = [];
    for (const b of message.game.battles as readonly { attack: Card; defense: Card }[]) {
        onTable.push(b.attack);
        if (covered(b)) onTable.push(b.defense);
    }
    for (const key of staleOptimisticKeysOnTable(s.motions, onTable, message.events)) {
        // The card comes off the entry, not out of the key: a key is
        // the kernel's packing of five fields and has none to read.
        const motion = s.motions.get(key);
        s.motions.delete(key);
        if (motion) s.positions.delete(getCardKey(motion.card));
    }
}

/**
 * The push's events MINUS the ones this client already animated as a prediction,
 * releasing each released prediction's tracking as it goes.
 *
 * An event is mine-already-played exactly when every card in it is under one of
 * my pending keys - the server may split a multi-card action into one event per
 * card, so the test is per card and the whole event is skipped only if all of
 * them match. The events that come back are what is left to animate; when
 * nothing is, the caller has only a board to commit.
 */
export function withoutConfirmedMotions(s: OptimisticState, serverEvents: any[]): ClientAnimationEvent[] {
    const nonOptimisticEvents: ClientAnimationEvent[] = [];

    serverEvents.forEach((serverEvent: any) => {
        // Check if ALL cards in this server event were optimistically animated
        if (!serverEvent.cards || serverEvent.cards.length === 0) {
            nonOptimisticEvents.push(serverEvent);
            return;
        }

        const allCardsOptimistic = serverEvent.cards.every((card: Card) =>
            s.motions.has(
                animEventKey(serverEvent.type, card, serverEvent.from_location, serverEvent.to_location, serverEvent.seat)));

        if (allCardsOptimistic) {
            // Clear the optimistic animations since server confirmed them
            serverEvent.cards.forEach((card: Card) => {
                s.motions.delete(
                    animEventKey(serverEvent.type, card, serverEvent.from_location, serverEvent.to_location, serverEvent.seat));

                // Also clear position tracking since server confirmed the move
                s.positions.delete(getCardKey(card));
            });
        } else {
            nonOptimisticEvents.push(serverEvent);
        }
    });

    return nonOptimisticEvents;
}

/** What the caller owes the screen once a push has been judged against my predictions. */
export interface ConflictResolution {
    /** The flights that carry doomed predictions home, in the order they were found. */
    revertEvents: ClientAnimationEvent[];
    /** Whether one of those flights is a pass going home (its board needs the server's own lead and shield). */
    passIsInvalid: boolean;
}

/**
 * Judge an arriving push against my unanswered moves, and settle them.
 *
 * Side effects are the point: doomed predictions lose their tracking here, cards
 * the push keeps are merged into every board it carries (`message` is mutated),
 * and the flights the caller must queue come back in `revertEvents`.
 */
export function resolveOptimisticConflicts(s: OptimisticState, message: IncomingPush): ConflictResolution {
    let revertEvents: ClientAnimationEvent[] = [];
    let passIsInvalid = false;

    if (message.events.length <= 0 || s.motions.size <= 0) {
        return { revertEvents, passIsInvalid };
    }

    // Get the FINAL server game state (last event's state shows the end result)
    const lastEventWithState = [...message.events].reverse().find((evt: any) => evt.game_state);

    if (!lastEventWithState) {
        return { revertEvents, passIsInvalid };
    }

    if (!lastEventWithState.game_state) {
        return { revertEvents, passIsInvalid };
    }

    const serverState: TableView = lastEventWithState.game_state;
    const mySeat = seatOf(serverState);

    // Check if server's final state already includes my optimistic cards
    // If so, they were accepted! Don't revert.
    const serverTableCards = tableCards(serverState);


    // Find MY optimistic cards (attacks, covers, pickups)
    const myOptimisticAttackCovers: Card[] = [];
    // Which of those are COVERS - the defender-capacity revert rule only
    // applies to attacks (see optimisticConflicts.ts).
    const myOptimisticCoverKeys = new Set<string>();
    const myOptimisticPickups: Card[] = [];

    // Queue revert events for every still-pending optimistic attack/cover
    // (skipping any already being reverted). Used by the several conflict
    // branches below that all need to roll these cards back to hand.
    const revertOptimisticAttackCovers = () => {
        myOptimisticAttackCovers.forEach(optCard => {
            const cardKey = getCardKey(optCard);

            if (s.reverting.has(cardKey)) {
                return;
            }
            s.reverting.add(cardKey);

            const visualPosition = s.positions.get(cardKey);
            const fromLocation = visualPosition?.location || 'table';

            revertEvents.push({
                type: 'revert',
                cards: [optCard],
                from_location: fromLocation as any,
                to_location: 'hand',
                seat: mySeat,
                is_revert: true,
                game_state: null as any
            });

            s.motions.delete(animEventKey('attack_pass', optCard, 'hand', 'table', mySeat));
        });
    };

    s.motions.forEach((motion) => {
        if (motion.seat !== mySeat) return;
        // Attacks and covers (hand → table)
        if ((motion.type === 'attack_pass' || motion.type === 'cover') &&
            motion.from === 'hand' && motion.to === 'table') {
            myOptimisticAttackCovers.push(motion.card);
            if (motion.type === 'cover') {
                myOptimisticCoverKeys.add(getCardKey(motion.card));
            }
        }
        // Pickups (table → hand)
        else if (motion.type === 'pickup' && motion.from === 'table' && motion.to === 'hand') {
            myOptimisticPickups.push(motion.card);
        }
    });

    // Check if server's final state already includes my optimistic attack/cover cards
    // If so, server accepted them - don't revert!
    const myOptimisticCardsAccepted = cardsIntersection(myOptimisticAttackCovers, serverTableCards);

    const serverAttackPasses = message.events.filter((evt: any) => evt.type === 'attack_pass');

    // ====== CHECK FOR OPTIMISTIC PASS CONFLICTS EARLY ======
    // Do this BEFORE merging, so invalid pass cards don't get baked into states
    // The seat the shield lands on once my pending pass stands, asked of the
    // KERNEL against this broadcast's own board rather than read off two seats
    // cached when the card was tapped. A board the broadcast has already
    // confirmed the pass on comes back holding the shield the server gave it;
    // one it has not comes back with the shield handed on. Either way this is
    // the defender the cards below are judged against, and there is no third
    // answer for a stale cache to supply.
    const passedOpen = passedBoard(s, serverState);
    if (passedOpen && message.events.length > 0 && serverAttackPasses.length > 0) {
        const nextDefenderId = passedOpen.defender;
        const finalGameState: TableView = message.game || serverState;

        // My still-pending hand-to-table cards (an attack and a pass are one
        // event type on the wire, so this is both).
        const passCards: Card[] = [];
        s.motions.forEach((motion) => {
            if (motion.type === 'attack_pass' && motion.seat === mySeat
                && s.positions.has(getCardKey(motion.card))) {
                passCards.push(motion.card);
            }
        });

        // The KERNEL decides (anim_plan.h anim_conflict_verdict), against the
        // NEXT defender's hand - the seat this pass hands the shield to. This
        // used to be an inline capacity subtraction, which had none of the
        // rule's precedence: a pass card the broadcast's own sweep carries off
        // is CLEAR and one standing on its opening table is KEEP, and reverting
        // either is the flicker c/src/anim_plan.h opens by describing.
        const passCardsToRevert = resolveConflictMotions(
            passCards.map((card) => ({ card, dest: CONFLICT_DEST.table })),
            {
                events: message.events,
                open: serverState,
                final: finalGameState,
                defenderSeat: nextDefenderId,
            }).revert;

        if (passCardsToRevert.length > 0) {
            passIsInvalid = true;

            {
                passCardsToRevert.forEach(card => {
                    const cardId = getCardKey(card);
                    s.reverting.add(cardId);
                });

                // Create revert event
                revertEvents.push({
                    type: 'revert',
                    cards: passCardsToRevert,
                    from_location: 'table',
                    to_location: 'hand',
                    seat: mySeat,
                    is_revert: true,
                    game_state: null as any // Will be set later
                });

                // The pass goes home, so it is pending nowhere: dropping the
                // cards' tracking is what says so (passStillPending reads it).
                passCardsToRevert.forEach(card => {
                    s.motions.delete(animEventKey('attack_pass', card, 'hand', 'table', mySeat));
                });

                // Remove pass cards from myOptimisticAttackCovers so they don't get merged
                passCardsToRevert.forEach(passCard => {
                    const idx = myOptimisticAttackCovers.findIndex(c =>
                        c.suit === passCard.suit && c.value === passCard.value
                    );
                    if (idx >= 0) {
                        myOptimisticAttackCovers.splice(idx, 1);
                    }
                });
            }
        }
    }

    // ====== CHECK FOR OPTIMISTIC ATTACK + SERVER PASS CONFLICTS ======
    // A pass is detected when the defender changes between states
    // Check if server events contain a pass that invalidates optimistic attacks
    const serverDefenderBefore = serverState?.defender;
    const serverDefenderAfter = (message.game || serverState)?.defender;
    const defenderChanged = serverDefenderBefore !== undefined &&
        serverDefenderAfter !== undefined &&
        serverDefenderBefore !== serverDefenderAfter;

    if (myOptimisticAttackCovers.length > 0 && defenderChanged && serverAttackPasses[0]) {
        const finalGameState: TableView = message.game || serverState;
        const newDefenderId = serverDefenderAfter; // After pass

        // Check 1: Did the pass make the attacker become the defender?
        if (newDefenderId !== undefined) {
            if (finalGameState.mySeat === newDefenderId) {
                // Revert all optimistic attacks
                revertOptimisticAttackCovers();

                // Remove from merge list
                myOptimisticAttackCovers.length = 0;
            }
        }

        // Check 2: can the new defender still take our in-flight attacks?
        // The same verdict, against the hand the pass just installed. The
        // inline version reverted the WHOLE set on a capacity failure, so a
        // card the broadcast itself showed on the table flew home red.
        if (myOptimisticAttackCovers.length > 0 && newDefenderId !== undefined) {
            const doomed = resolveConflictMotions(
                myOptimisticAttackCovers.map((card) => ({
                    card,
                    dest: CONFLICT_DEST.table,
                    isCover: myOptimisticCoverKeys.has(getCardKey(card)),
                })),
                {
                    events: message.events,
                    open: serverState,
                    final: finalGameState,
                    defenderSeat: newDefenderId,
                }).revert;

            if (doomed.length > 0) {
                revertOptimisticAttackCovers();

                // Remove from merge list
                myOptimisticAttackCovers.length = 0;
            }
        }
    }

    // Handle optimistic pickup conflicts
    if (myOptimisticPickups.length > 0) {
        // The KERNEL decides WHICH pickups are doomed (anim_plan.h
        // anim_conflict_verdict). A pickup's cards landed in MY HAND, so the
        // standing set they are judged against is my hand on the broadcast's
        // opening board, not its table - the one input the inline version had
        // no way to express. A pickup the broadcast confirms is KEEP.
        //
        // BOTH revert AND clear fly back, and the difference from the
        // attack/cover branch is the transport, not the rule. CLEAR means
        // "the incoming stream animates this card itself", which spares a
        // flight only when the card is already standing where that stream
        // replays it from. For an attack it is: the card is on the table and
        // the sweep lifts it off the table. For a PICKUP it is not: the card
        // is in my hand, and the sweep carries it from the TABLE to the
        // discard. iMessage has no such gap because a chain rebases the board
        // to the state it vouches for before replaying; the web has no rebase
        // step, so the return flight IS its way of standing on that board.
        // Dropping it would leave the card in my hand while the trash
        // animated an empty table, then vanish it when the final state lands.
        // The FLIGHT is the caller's - anim_plan.h says so - and the web's
        // caller needs this one.
        const pickupVerdicts = resolveConflictMotions(
            myOptimisticPickups.map((card) => ({ card, dest: CONFLICT_DEST.hand })),
            {
                events: message.events,
                open: serverState,
                final: message.game || serverState,
                defenderSeat: -1,
                pendingAttacks: 0,
            });
        const pickupCardsToRevert = [...pickupVerdicts.revert, ...pickupVerdicts.clear];

        if (pickupCardsToRevert.length > 0) {
            // Mark all cards as reverting and clear tracking
            pickupCardsToRevert.forEach(card => {
                const cardKey = getCardKey(card);
                s.reverting.add(cardKey);

                // Clear tracking
                s.motions.delete(animEventKey('pickup', card, 'table', 'hand', mySeat));
            });

            // Create SINGLE revert event with ALL cards
            revertEvents.push({
                type: 'revert',
                cards: pickupCardsToRevert, // ALL cards in one event
                from_location: 'hand',
                to_location: 'table',
                seat: mySeat,
                is_revert: true,
                game_state: null as any // Will be set later
            });
        }
    }

    if (myOptimisticAttackCovers.length > 0 && myOptimisticCardsAccepted.length === 0) {
        // Server didn't include our optimistic cards yet. Decide per card whether
        // each was genuinely never accepted (revert to hand) or is simply not yet
        // confirmed on THIS (possibly concurrent / pre-our-commit) broadcast and
        // should be kept (merged) - see optimisticConflicts.ts. This is the same
        // decision the deployed client and the e2e suite both exercise.
        const { revert: cardsToRevert, merge: cardsToMerge, clear: cardsToClear } = resolveUnconfirmedAttackCovers(
            myOptimisticAttackCovers,
            serverState,
            message.events,
            message.game || serverState,
            myOptimisticCoverKeys,
        );

        // Cards that were accepted then swept off the table by this broadcast's
        // own pickup/trash: drop their optimistic tracking with NO revert - the
        // clear event animates them off the table (was the "someone picked up my
        // card and it flew back to my hand" flicker).
        cardsToClear.forEach((card: Card) => {
            const cardKey = getCardKey(card);
            s.motions.delete(animEventKey('attack_pass', card, 'hand', 'table', mySeat));
            s.motions.delete(animEventKey('cover', card, 'hand', 'table', mySeat));
            s.positions.delete(cardKey);
        });

        if (cardsToRevert.length > 0) {
            // Create revert animation for the cards that were genuinely too slow.
            cardsToRevert.forEach((card: Card) => {
                const cardKey = getCardKey(card);

                if (s.reverting.has(cardKey)) {
                    return;
                }

                s.reverting.add(cardKey);

                // Get where this card currently is visually
                const visualPosition = s.positions.get(cardKey);
                const fromLocation = visualPosition?.location || 'table';

                revertEvents.push({
                    type: 'revert',
                    cards: [card],
                    from_location: fromLocation as any,
                    to_location: 'hand',
                    seat: mySeat,
                    is_revert: true,
                    message: 'Attack invalidated by earlier attack'
                });

                // Clear from optimistic tracking
                s.motions.delete(animEventKey('attack_pass', card, 'hand', 'table', mySeat));
            });
        }

        if (cardsToMerge.length > 0) {
            // Each card with the attack it covers, if it is a cover.
            const pending = cardsToMerge.map((card: Card) => ({
                card,
                target: s.positions.get(getCardKey(card))?.target_card ?? null,
            }));

            // Keep the optimistic cards on ALL boards (events + final): the kernel lays
            // each one the board does not already show, over its target or as an
            // attack, and takes it out of my hand. FIRST my pending pass, asked of the
            // kernel per board: a board this broadcast has already confirmed the pass
            // on is left alone, and one it has not gets the shield handed on, so the
            // board a card is kept on never shows my pass's cards under the server's
            // old shield. The test the two used to share - "does this message contain
            // MY pass" - was a guess at that from the outside.
            const keep = (board: TableView): TableView => {
                const next: TableView | null = passedBoard(s, board) ?? board;
                return keepPending(next, pending) ?? board;
            };
            for (const evt of message.events) if (evt.game_state) evt.game_state = keep(evt.game_state);
            if (message.game) message.game = keep(message.game);
        }
    }

    return { revertEvents, passIsInvalid };
}
