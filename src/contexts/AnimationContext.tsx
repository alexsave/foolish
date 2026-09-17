import React, { createContext, useContext, useEffect, useState, useRef, useCallback } from 'react';
import { useServer, useServerActions } from './ServerContext';
import { useAuth } from './AuthContext';
import { useParams } from 'next/navigation';
import supabase from '../backend/Connector';
import { validateActionWire } from '../utils/gameValidation';
import { encodeAction } from '@sdk/ts/wire/awire.ts';
import { clientTable } from '@sdk/ts/table/client_table.ts';
import { pushToSequence } from '../state/pushSequence';
import { covered, rulesOf, type TableView, type ViewCard } from '../state/view';
import { keepPending, lifted, optimisticBoard, returnedToHand, tableOf, turnedBoard, withdrawn } from '../state/clientBoards';
import { base64ToBytes } from '@sdk/ts/wire/bytes.ts';
import { getTableCards, cardsIntersection, getCardKeyOwner, getCardKey } from '../utils/animationUtils';
import { animationFeed } from '../state/animationFeed';
import { staleOptimisticKeysOnTable } from '../state/optimisticAnimation';
import { resolveUnconfirmedAttackCovers, resolveConflictMotions, CONFLICT_DEST } from '../state/optimisticConflicts';
import { optimisticOverlay } from '../state/optimisticOverlay';
import { shouldDropStaleSequence } from '../state/clientReconcile';
import { noteAuthoritativeVersion } from '../state/authoritativeVersion';
import { ANIM_CONFLICT_REVERT, animEventKey, animReversalOrder } from '@sdk/ts/wasm/bots.ts';
import { useAnimationRun } from '../state/useAnimationRun';

// Bot bump timeout - 20 seconds of no animations (currently unused)
// const BOT_BUMP_TIMEOUT = 20000;

type Card = ViewCard;

// Every board this pipeline commits is a board it was given - a push's step, a
// replay frame - or one the kernel made from one (src/state/clientBoards.ts): my
// pending cards kept on a push's boards, the lead and shield of my pending pass,
// the board a revert flies home to. Nothing here edits a board.

// The board's own seat, if the board has one: the seat an event of mine is keyed by.
const seatOf = (v: TableView | null | undefined): number | undefined =>
    v && v.mySeat >= 0 ? v.mySeat : undefined;

// The places a flight's card is drawn at while it flies, by the owner key the
// page's CardFace names them with (src/components/GameDisplay/CardFace.tsx): a
// seat's hand, the table, the flipped slot. The card is hidden at the place it
// leaves AND the place it lands on - a confirmation or a board committed before
// the flight lands shows it there already - so the flight is the one card the
// page draws. A card leaving the deck may be the flipped trump, which the stock
// shows under the deck.
const flightPlaces = (from: string | undefined, to: string | undefined, seat: number | undefined): (number | string)[] => {
    const places: (number | string)[] = [];
    for (const loc of [from, to]) {
        if (loc === 'hand' && seat !== undefined) places.push(seat);
        else if (loc === 'table' || loc === 'flipped') places.push(loc);
    }
    if (from === 'deck') places.push('flipped');
    return places.filter((p, i) => places.indexOf(p) === i);
};

// A MOVE OF MINE THE SERVER HAS NOT CONFIRMED. The map of these is keyed by the
// kernel's own dedup key (c/src/anim_plan.h anim_event_key): two events collide
// iff they name the same (type, card, from, to, seat), and the seat stands in
// for the player id because a plan is per viewer. The record carries the fields
// back, so nothing here ever takes a key apart - which is what the key it
// replaced existed for. That was a JSON.stringify of these same five fields,
// JSON.parse'd back out in five places, i.e. a byte layout TypeScript knew.
interface PendingMotion {
    type: string;
    card: Card;
    from: string;
    to: string;
    /** The acting seat, or undefined for a board with no seat of its own. */
    seat?: number;
    /** When it was predicted, for the sweep that drops motions nobody answered. */
    at: number;
}

interface ClientAnimationEvent  {
    type: 'magic_transition' | 'deal' | 'flipped' | 'defender_move' | 'attack_pass' | 'cover' | 'pickup' | 'discard' | 'out' | 'refill' | 'cards_to_trash' | 'revert';
    seat?: number;   // the acting seat
    cards?: readonly Card[];
    from_location?: 'deck' | 'hand' | 'table' | 'discard';
    to_location?: 'deck' | 'hand' | 'table' | 'discard' | 'flipped';
    target_card?: Card;
    target_cards?: readonly Card[]; // For multi-card cover animations
    battle_index?: number;
    message?: string;
    game_state?: TableView; // the board after this event
    is_revert?: boolean; // CLIENT-ONLY: flag for reverted optimistic animations
    // CLIENT-ONLY: whether this step's board is still worth committing when its
    // flight lands. A predicted move's board rides its own flight (there is no
    // second timer for it any more), and a refusal that arrives mid-flight must
    // stop it landing - otherwise the board appears and the revert takes it away
    // one frame later. Only a prediction carries one; a push's board is truth.
    commit_if?: () => boolean;
    // CLIENT-ONLY: a board this step only knows at its LANDING. A prediction's
    // board is the kernel's edit of whatever is on screen when its flight lands,
    // not of what was on screen when the card was tapped: a broadcast can commit
    // fresher state inside that window, and a board derived at tap time would
    // write the stale table and hand back over it.
    commit_board?: () => TableView | null;
}

interface AnimationContextType {
    isAnimating: boolean;
    currentAnimation: ClientAnimationEvent | null;
    /** How long the flight on screen lasts, from the kernel's plan: the number
     *  the overlay's CSS transition is written with. 0 when nothing is flying. */
    flightMs: number;
    // Cards currently flying from the deck pile. Drives the visible pile size.
    // Drops BEFORE the animation starts and resets when the snapshot commits.
    inFlightFromDeck: number;
    // Subset of inFlightFromDeck that's headed to the flipped slot - these
    // are still "in the deck system" so they count toward the badge total.
    inFlightToFlipped: number;
    // `owner`: the seat whose cards these are, or a place's own key ('table', 'flipped').
    getCardAnimationState: (card: Card, owner?: number | string) => {
        isAnimating: boolean;
        animationType: string | null;
        progress: number;
        fromLocation: string | null;
        toLocation: string | null;
    };
    // Game action methods that handle optimistic animations + server calls
    attack: (cards: Card[]) => Promise<{ game_id: string }>;
    pass: (cards: Card[]) => Promise<{ game_id: string }>;
    pickup: () => Promise<{ game_id: string }>;
    cover: (coverCards: Card[], attackCards: Card[]) => Promise<{ game_id: string }>;
    good: () => Promise<{ game_id: string }>;
    /** Drop everything queued or in flight, without committing pending
     *  states. Used by the replay player when seeking; a live game never
     *  needs it (the server stream is the only truth there). */
    resetAnimations: () => void;
}

// Exported so the tutorial can re-provide a value that overrides the action
// methods (attack/pass/pickup/cover/good) to drive its scripted playthrough,
// while still spreading the real animation state (isAnimating, etc.).
export const AnimationContext = createContext<AnimationContextType | null>(null);

// Compact content fingerprint of a sequence's events, for backup dedup (the
// primary key is sequence_id). The old code did JSON.stringify(events), which
// serialized each event's entire embedded `game_state` - the heaviest part of the
// payload - on every received message just to compare it. This signature captures
// the move-defining fields (type, player, from/to, cards, battle index) PLUS a few
// O(1) scalars off game_state (deck size, table size, own hand size) so two
// same-shaped-but-distinct sequences - e.g. two single-card refills at different
// deck sizes - still hash differently, the way the full stringify did, at a tiny
// fraction of the cost.
const eventsSignature = (events: any[]): string =>
    events
        .map((e) => {
            const gs = e.game_state;
            return [
                e.type,
                e.seat ?? '',
                e.from_location ?? '',
                e.to_location ?? '',
                e.battle_index ?? '',
                (e.cards ?? []).map((c: Card) => `${c.suit}-${c.value}`).join(','),
                // cheap state discriminators (no deep serialization)
                gs?.deckCount ?? '',
                gs?.battles.length ?? '',
                gs && gs.mySeat >= 0 ? gs.myHand.length : '',
            ].join('|');
        })
        .join(';');

// Check if any bot can possibly move on the current board. Turn eligibility
// lives in ONE place - the kernel's should_bot_act rule, asked of the board
// (ViewRules.bot_to_move; a hand-rolled copy here used to count a said-good
// attacker as movable while uncovered attacks remained, keeping the poll-bump
// timer firing for nobody).
const canBotMove = (view: TableView | undefined): boolean =>
    !!view && view.seats.length > 0 && rulesOf(view).botToMove;

export const AnimationProvider = ({ children }: { children: React.ReactNode }) => {
    // Actions come from the stable actions context (identity never changes);
    // only the state this provider genuinely needs comes from the state context.
    const serverActions = useServerActions();
    const { updateGameState } = serverActions;
    const { views: games, game_id } = useServer();
    const { user_id } = useAuth();
    const url_game_id = useParams<{ game_id: string }>().game_id?.toLowerCase();

    // The animation state the page renders from comes from the frame loop
    // (useAnimationRun, below): the kernel holds the timing, so nothing about a
    // run is state this file declares.
    const pendingCompletionCallbackRef = useRef<(() => void) | null>(null);
    const remainingSequenceEventsRef = useRef<number>(0);

    // Bot bump timer ref
    const botBumpTimerRef = useRef<NodeJS.Timeout | null>(null);

    // Track if there have been bot moves in the last interval
    const hasBotMovedRef = useRef<boolean>(false);

    // Ref to track current game state (avoids stale closure in interval)
    const currentGameRef = useRef<typeof games[string] | undefined>(undefined);
    // All loaded games, ref-mirrored for the feed subscription callback (the
    // packed-envelope decode needs the roster of the game the message names,
    // not the possibly-stale `games` closure the effect captured).
    const gamesRef = useRef(games);

    // Keep track of processed sequence IDs and event content to avoid duplicates
    const processedSequenceIds = useRef<Set<string>>(new Set());
    const processedEventContent = useRef<Set<string>>(new Set());

    // Store the current game ID for this animation sequence
    const currentGameIdRef = useRef<string | null>(null);

    // Track optimistically triggered animations to avoid server duplicates.
    // The kernel's dedup key -> what I predicted (see PendingMotion).
    const optimisticAnimations = useRef<Map<number, PendingMotion>>(new Map());

    // Remember a move of mine as pending, under the kernel's key for it.
    const rememberPending = (type: string, card: Card, from: string, to: string, seat: number | undefined, at: number): void => {
        optimisticAnimations.current.set(animEventKey(type, card, from, to, seat), { type, card, from, to, seat, at });
    };

    // Track cards that are currently being reverted to avoid duplicate revert animations
    const revertingCards = useRef<Set<string>>(new Set());

    // Track visual positions of optimistically animated cards (for accurate revert animations)
    // Map of cardKey -> { location: 'table' | 'hand', seat, target_card?: Card, battle_index?: number }
    const optimisticCardPositions = useRef<Map<string, { location: string, seat?: number, target_card?: Card, battle_index?: number }>>(new Map());

    // MY PASS THE SERVER HAS NOT CONFIRMED, as the action wire I sent it.
    //
    // Three places used to ask the same question - where does the shield stand on
    // a board my pending pass has not been confirmed on - and all three answered
    // it from two seat numbers cached at the moment of the tap. A cached board is
    // a board that can disagree with the one it is imposed on, and the way it
    // disagreed was the stutter: a card out, home in red, and out again.
    //
    // The wire is smaller state and better-shaped state, because the kernel
    // answers the whole question from it (c/src/client_table.c
    // client_optimistic_apply). A board whose table already shows the pass's
    // cards is left exactly as the server wrote it - "a move none of whose cards
    // is new has already happened, so a pass hands the shield on no further" -
    // and a board that does not show them gets the shield handed on. So no site
    // below prefers my guess or the server's board; the kernel says which a board
    // is, per board, every time it is asked.
    const pendingPass = useRef<{ wire: Uint8Array; cards: readonly Card[]; seat?: number } | null>(null);

    // The pass is pending exactly while the optimistic map still holds one of its
    // cards. Every path that resolves a prediction already releases it there -
    // the confirming broadcast's dedup partition, the version gate, the conflict
    // reverts, a refusal, the sweep - so the pass needs no clearing discipline of
    // its own, which is the discipline the cached seats kept getting wrong.
    const passStillPending = (): boolean => {
        const p = pendingPass.current;
        if (!p) return false;
        const live = p.cards.some((c) => optimisticAnimations.current.has(animEventKey('attack_pass', c, 'hand', 'table', p.seat)));
        if (!live) pendingPass.current = null;
        return live;
    };

    /** `board` with my still-unconfirmed pass standing on it, asked of the kernel;
     *  null when no pass is pending or the kernel refuses to change the board. */
    const passedBoard = (board: TableView | null | undefined): TableView | null =>
        board && passStillPending() ? optimisticBoard(board, pendingPass.current!.wire) : null;

    // Expose the local player's live optimistic table cards to the REST load path,
    // so a reconnect resync re-applies them instead of momentarily wiping them
    // (the "vanish then reappear" glitch). Derived on demand from the live
    // position tracking, so it's always current.
    useEffect(() => optimisticOverlay.register(() => {
        const out: { card: Card; target?: Card | null }[] = [];
        optimisticCardPositions.current.forEach((pos, cardKey) => {
            const [suit, value] = cardKey.split('-').map(Number);
            if (Number.isFinite(suit) && Number.isFinite(value)) {
                out.push({ card: { suit, value }, target: pos.target_card ?? null });
            }
        });
        return out;
    }), []);

    // Highest committed games.version we've applied from a live broadcast. Live
    // sequences are fired un-awaited by the server over per-call channels, so under
    // realtime latency they can arrive out of order; we drop any whose version is
    // <= this one (strictly superseded - each sequence carries the full resulting
    // state). null until the first versioned sequence; reset when the game changes.
    const lastAppliedVersionRef = useRef<number | null>(null);
    const gateGameRef = useRef<string | undefined>(undefined);
    // Reset the gate when switching games, and seed/raise it from authoritative
    // REST loads (initial load and the post-reconnect resync). Live broadcasts
    // advance it higher during play; we never lower it, so a late in-flight stale
    // broadcast arriving after a resync is still dropped.
    useEffect(() => {
        if (gateGameRef.current !== url_game_id) {
            gateGameRef.current = url_game_id;
            lastAppliedVersionRef.current = null;
        }
        const v = url_game_id ? games[url_game_id]?.version : undefined;
        if (typeof v === 'number') {
            lastAppliedVersionRef.current = lastAppliedVersionRef.current === null
                ? v : Math.max(lastAppliedVersionRef.current, v);
            // Share it with ServerContext so an outgoing move can be stamped with
            // the version the client composed it against (the round guard).
            noteAuthoritativeVersion(url_game_id, lastAppliedVersionRef.current);
        }
    }, [url_game_id, games]);

    // Keep currentGameRef in sync with latest game state
    useEffect(() => {
        currentGameRef.current = url_game_id ? games[url_game_id] : undefined;
        gamesRef.current = games;
    }, [url_game_id, games]);

    // Start bot bump timer when component mounts and game is loaded
    useEffect(() => {
        if (!url_game_id) {
            return;
        }

        // Check if there are any AI players in the game
        const currentGame = games[url_game_id];
        const hasAIPlayers = currentGame?.seats.some(seat => seat.isAi) || false;

        // Only start timer if there are AI players
        if (!hasAIPlayers) {
            return;
        }

        // Start interval that checks every 5 seconds
        const intervalId = setInterval(() => {
            // Check if there have been bot moves in the last interval
            if (!hasBotMovedRef.current) {
                // Check if any bot can actually move in current state (use ref for fresh data)
                const botCanMove = canBotMove(currentGameRef.current);
                if (!botCanMove) {
                    // No bot can move - we're waiting for a human player, skip bump
                    return;
                }

                // No bot moves but a bot could move - nudge the bot loop (folded into
                // the unified `action` endpoint as type:'bump'; was the bot_bump fn)
                supabase.functions.invoke('action', {
                    body: { game_id: url_game_id, type: 'bump' }
                }).catch(error => {
                    console.error('Bot bump failed:', error);
                });
            }

            // Reset the flag for the next interval
            hasBotMovedRef.current = false;
        }, 5000); // 5 seconds

        // Store the interval ID for cleanup
        botBumpTimerRef.current = intervalId as any;

        // Cleanup on unmount
        return () => {
            if (botBumpTimerRef.current) {
                clearInterval(botBumpTimerRef.current);
                botBumpTimerRef.current = null;
            }
        };
    // Re-run when game data loads or players change (bot might be added)
    // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [url_game_id, url_game_id ? games[url_game_id]?.seats.length : 0]);

    // Clear OLD optimistic animations every 5 seconds (older than 30 seconds)
    useEffect(() => {
        const interval = setInterval(() => {
            const now = Date.now();
            const threshold = 30000; // 30 seconds

            // Only clear animations older than 30 seconds
            const toDelete: number[] = [];
            optimisticAnimations.current.forEach((motion, key) => {
                if (now - motion.at > threshold) {
                    toDelete.push(key);
                }
            });

            toDelete.forEach(key => {
                optimisticAnimations.current.delete(key);
            });

        }, 5000); // Check every 5 seconds

        return () => clearInterval(interval);
    }, []);

    // Consume the animation feed. The transport lives elsewhere: live games
    // mount RealtimeAnimationFeed (src/state/) which republishes the supabase
    // broadcast channel into the bus; the replay screen publishes synthesized
    // sequences from a decoded replay integer. Either way the messages are
    // identical in shape, so this provider animates both.
    useEffect(() => {
        return animationFeed.subscribe((message) => {
            handleAnimationMessage(message);
        });
        // handleAnimationMessage closes over stable refs + setState updaters;
        // resubscribing on these deps mirrors the old channel effect.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [user_id, url_game_id]);

    const resolveOptimisticConflicts = (message: any) => {
        let revertEvents: ClientAnimationEvent[] = [];
        let passIsInvalid = false;

        if (message.events.length <= 0 || optimisticAnimations.current.size <= 0) {
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
        const serverTableCards = getTableCards(serverState);


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

                if (revertingCards.current.has(cardKey)) {
                    return;
                }
                revertingCards.current.add(cardKey);

                const visualPosition = optimisticCardPositions.current.get(cardKey);
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

                optimisticAnimations.current.delete(animEventKey('attack_pass', optCard, 'hand', 'table', mySeat));
            });
        };

        optimisticAnimations.current.forEach((motion) => {
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
        const passedOpen = passedBoard(serverState);
        if (passedOpen && message.events.length > 0 && serverAttackPasses.length > 0) {
            const nextDefenderId = passedOpen.defender;
            const finalGameState: TableView = message.game || serverState;

            // My still-pending hand-to-table cards (an attack and a pass are one
            // event type on the wire, so this is both).
            const passCards: Card[] = [];
            optimisticAnimations.current.forEach((motion) => {
                if (motion.type === 'attack_pass' && motion.seat === mySeat
                    && optimisticCardPositions.current.has(getCardKey(motion.card))) {
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
                        revertingCards.current.add(cardId);
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
                        optimisticAnimations.current.delete(animEventKey('attack_pass', card, 'hand', 'table', mySeat));
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
                    revertingCards.current.add(cardKey);

                    // Clear tracking
                    optimisticAnimations.current.delete(animEventKey('pickup', card, 'table', 'hand', mySeat));
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
                optimisticAnimations.current.delete(animEventKey('attack_pass', card, 'hand', 'table', mySeat));
                optimisticAnimations.current.delete(animEventKey('cover', card, 'hand', 'table', mySeat));
                optimisticCardPositions.current.delete(cardKey);
            });

            if (cardsToRevert.length > 0) {
                // Create revert animation for the cards that were genuinely too slow.
                cardsToRevert.forEach((card: Card) => {
                    const cardKey = getCardKey(card);

                    if (revertingCards.current.has(cardKey)) {
                        return;
                    }

                    revertingCards.current.add(cardKey);

                    // Get where this card currently is visually
                    const visualPosition = optimisticCardPositions.current.get(cardKey);
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
                    optimisticAnimations.current.delete(animEventKey('attack_pass', card, 'hand', 'table', mySeat));
                });
            }

            if (cardsToMerge.length > 0) {
                // Each card with the attack it covers, if it is a cover.
                const pending = cardsToMerge.map((card: Card) => ({
                    card,
                    target: optimisticCardPositions.current.get(getCardKey(card))?.target_card ?? null,
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
                    const next: TableView | null = passedBoard(board) ?? board;
                    return keepPending(next, pending) ?? board;
                };
                for (const evt of message.events) if (evt.game_state) evt.game_state = keep(evt.game_state);
                if (message.game) message.game = keep(message.game);
            }
        }

        return { revertEvents, passIsInvalid };
    }

    // Packed envelopes we can't decode (unknown game, corrupt bytes, roster
    // desync): refetch the authoritative state once instead of dropping
    // silently forever. The refetch re-checks the landed version against the
    // broadcast's - a load that was already in flight when the broadcast
    // committed can return an OLDER state (its read predates the commit), so
    // one more load is chained in that case.
    const packedRefetchInFlight = useRef<Set<string>>(new Set());
    const refetchForEnvelope = (gid: string | undefined, minVersion: number | undefined) => {
        if (!gid || packedRefetchInFlight.current.has(gid)) return;
        packedRefetchInFlight.current.add(gid);
        serverActions.loadGame(gid)
            .then(() => {
                const landed = gamesRef.current[gid];
                if (landed && minVersion !== undefined && (landed.version ?? 0) < minVersion) {
                    return serverActions.loadGame(gid);
                }
            })
            .catch(() => { /* resubscribe resync covers persistent failures */ })
            .finally(() => packedRefetchInFlight.current.delete(gid));
    };

    // Packed broadcast envelope {t:'as2' | 'as3', s, v, b, game_id, r?} -> the
    // sequence this pipeline plays (docs/PACKED_WIRE_CUTOVER.md). This is the
    // client's render-boundary read for live broadcasts: the kernel reads the
    // bytes (sdk/ts/table/client_table.ts) into boards, src/state/pushSequence.ts
    // names each step's event, and the EXISTING pipeline (version gate, dedup,
    // optimistic-conflict resolution) runs unchanged on the result.
    //
    // Who sits where: an as3 push that changed the roster carries it. Otherwise
    // the table's identity the kernel kept from the last envelope or roster push
    // it read for this game names the seats - or, from a server that still
    // sends as2 lobby broadcasts, the JSON roster beside them (`r`), which is
    // exactly the case a kept identity would be stale for. `m` (message prose)
    // is ignored: no component renders an event's message (Q7).
    const decodePackedEnvelope = (m: any): any | null => {
        if (typeof m.b !== 'string') return null;
        const gid = typeof m.game_id === 'string' ? m.game_id : url_game_id;
        const version = typeof m.v === 'number' ? m.v : undefined;
        const table = clientTable();
        let identity: 'kept' | Uint8Array = 'kept';
        if (m.r && Array.isArray(m.r.players) && gid) {
            const built = table.identityFromSeats(gid, m.r.name ?? '', m.r.players.map((p: any) => ({
                id: String(p.player_id), name: String(p.name ?? ''), isAi: p.is_ai === true,
            })));
            if (built) identity = built;
        }
        let read = null;
        try {
            read = table.readPush(base64ToBytes(m.b), { as3: m.t === 'as3', gameId: gid, version, identity });
        } catch (e) {
            console.error('packed animation envelope decode failed:', e);
        }
        // Unreadable bytes, or a push the kernel could not name the seats of
        // (no identity for this game yet, or one whose seats the push's boards
        // do not have - the roster changed while this client was away): fetch
        // the authoritative state and drop this sequence. The load carries the
        // fresh roster.
        if (!read || read.final.seats.length === 0 || read.final.seats[0].id === '') {
            refetchForEnvelope(gid, version);
            return null;
        }
        const decoded = pushToSequence(read);
        return {
            type: 'animation_sequence',
            sequence_id: m.s,
            timestamp: Date.now(),
            version: m.v,
            events: decoded.events,
            game: decoded.game,
        };
    };

    // Handle animation messages from real-time channel
    const handleAnimationMessage = (message: any) => {
        if (message && (message.t === 'as2' || message.t === 'as3')) {
            message = decodePackedEnvelope(message);
            if (!message) return;
        }
        if (!message.events || !Array.isArray(message.events)) {
            return;
        }

        // Monotonic ordering gate. Live broadcasts carry the committed games.version;
        // drop any that arrives at or below the newest version we've already applied
        // (stale/out-of-order/duplicate). Each sequence carries the full resulting
        // state, so dropping a superseded one loses nothing. Replay-synthesized
        // sequences have no version and are never gated.
        const incomingVersion = typeof message.version === 'number' ? message.version : null;
        if (incomingVersion !== null) {
            if (shouldDropStaleSequence(lastAppliedVersionRef.current, incomingVersion)) {
                return;
            }
            lastAppliedVersionRef.current = incomingVersion;
            // A live broadcast is the freshest authoritative version - feed the
            // move-stamping store so the next tap carries the current round.
            noteAuthoritativeVersion(gateGameRef.current, incomingVersion);

            // Release any of my optimistic cards that this AUTHORITATIVE state
            // confirms are on the table BUT whose confirming broadcast was
            // dropped by the version gate (i.e. NOT named by this broadcast's
            // own events). Cards this broadcast DOES name are deliberately left
            // for the per-event dedup below - releasing them here first would
            // make their own confirming event look un-optimistic and animate a
            // second time (the double-play bug). message.game is the pristine
            // server state here (resolveOptimisticConflicts hasn't injected yet).
            if (message.game?.battles && optimisticAnimations.current.size > 0) {
                const tableCards: Card[] = [];
                for (const b of message.game.battles as readonly { attack: Card; defense: Card }[]) {
                    tableCards.push(b.attack);
                    if (covered(b)) tableCards.push(b.defense);
                }
                for (const key of staleOptimisticKeysOnTable(optimisticAnimations.current, tableCards, message.events)) {
                    // The card comes off the entry, not out of the key: a key is
                    // the kernel's packing of five fields and has none to read.
                    const motion = optimisticAnimations.current.get(key);
                    optimisticAnimations.current.delete(key);
                    if (motion) optimisticCardPositions.current.delete(getCardKey(motion.card));
                }
            }
        }

        // Store the game ID for use during animations
        currentGameIdRef.current = message.game.gameId;

        // Check for duplicate sequence_id FIRST (before checking optimistic events)
        const sequenceId = message.sequence_id;

        if (processedSequenceIds.current.has(sequenceId)) {
            return;
        }

        // Also check event content as backup (compact signature, not a full
        // JSON.stringify of the events + their embedded game snapshots).
        const eventsString = eventsSignature(message.events);

        if (processedEventContent.current.has(eventsString)) {
            return;
        }

        // Mark as processed early to prevent race conditions
        processedSequenceIds.current.add(sequenceId);
        processedEventContent.current.add(eventsString);

        // Check EACH event individually to see if it was optimistically animated
        // Only skip the events that are optimistic, not the entire sequence
        const serverEvents = message.events;

        // Filter out optimistic events, keeping only non-optimistic ones
        const nonOptimisticEvents: ClientAnimationEvent[] = [];
        const optimisticEventIndices: number[] = [];

        serverEvents.forEach((serverEvent: any, eventIndex: number) => {
            // Check if ALL cards in this server event were optimistically animated
            if (!serverEvent.cards || serverEvent.cards.length === 0) {
                nonOptimisticEvents.push(serverEvent);
                return;
            }

            const allCardsOptimistic = serverEvent.cards.every((card: Card) =>
                optimisticAnimations.current.has(
                    animEventKey(serverEvent.type, card, serverEvent.from_location, serverEvent.to_location, serverEvent.seat)));

            if (allCardsOptimistic) {
                optimisticEventIndices.push(eventIndex);
                // Clear the optimistic animations since server confirmed them
                serverEvent.cards.forEach((card: Card) => {
                    optimisticAnimations.current.delete(
                        animEventKey(serverEvent.type, card, serverEvent.from_location, serverEvent.to_location, serverEvent.seat));

                    // Also clear position tracking since server confirmed the move
                    optimisticCardPositions.current.delete(getCardKey(card));
                });
            } else {
                nonOptimisticEvents.push(serverEvent);
            }
        });

        // If ALL events were optimistic, just update state and return
        if (nonOptimisticEvents.length === 0) {
            if (message.game) {
                updateGameState(message.game.gameId, message.game);
            }
            return;
        }

        // Otherwise, continue with non-optimistic events
        message.events = nonOptimisticEvents;

        // Clean up old sequence IDs to prevent memory leaks (keep only last 50)
        // Event content is cleared after each sequence, so no cleanup needed there
        if (processedSequenceIds.current.size > 50) {
            const ids = Array.from(processedSequenceIds.current);
            processedSequenceIds.current = new Set(ids.slice(-25));
        }

        // CONFLICT DETECTION: Check if server events invalidate our optimistic animations
        //const revertEvents: ClientAnimationEvent[] = [];

        // Get current displayed state (what the user sees with optimistic updates)

        const resolveResult = resolveOptimisticConflicts(message);
        const revertEvents: ClientAnimationEvent[] = resolveResult.revertEvents;
        const passIsInvalid = resolveResult.passIsInvalid;

        // Store the completion callback to update final game state
        pendingCompletionCallbackRef.current = () => {
            if (!message.game) {
                return;
            }

            // The board this sequence settles on, with my pass on it if the server
            // has not taken it yet. The kernel decides which of those a board is
            // (client_optimistic_apply): a final board whose table already shows
            // the pass's cards is the server's answer and comes back untouched,
            // and one that does not is a board my move is still in the air over,
            // so it gets the shield handed on. Three tests used to stand here -
            // is this message mine, does the server agree with my two cached
            // seats, and is it mine but disagreeing - to pick between trusting my
            // guess and trusting the server. There is no guess left to trust.
            message.game = passedBoard(message.game) ?? message.game;

            updateGameState(message.game.gameId, message.game);
        };
        remainingSequenceEventsRef.current = message.events.length + revertEvents.length;

        if (revertEvents.length === 0) {
            // Queue all events from the sequence
            enqueue(message.events);
            return;
        }

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
        enqueue([...reversal.flat().map((i) => revertEvents[i]), ...message.events]);
    };


    // ONE FRAME LOOP, and the kernel answers it (src/state/useAnimationRun.ts).
    // What is left here is what a landing MEANS - which board it commits, what
    // tracking it releases, when a sequence's final board is the truth - because
    // that is about the game, and the loop is about time.
    const {
        isAnimating, currentAnimation, flightMs, inFlightFromDeck, inFlightToFlipped,
        animatingCards, enqueue, reset: resetRun,
    } = useAnimationRun<ClientAnimationEvent>({
        board: () => currentGameRef.current,
        placesOf: (step) => flightPlaces(step.from_location, step.to_location, step.seat),
        keyOf: (card, place) => getCardKeyOwner(card, place),
        onLanded: (step) => {
            // A PREDICTION'S BOARD IS MADE AT ITS LANDING, from whatever is on
            // screen then - a broadcast can commit fresher state inside the
            // flight, and a board derived at tap time would write the stale
            // table and hand back over it.
            const board = step.commit_board ? step.commit_board() : step.game_state;
            const commitGameId = currentGameIdRef.current ?? board?.gameId;
            if (board && commitGameId && (step.commit_if?.() ?? true)) {
                updateGameState(commitGameId, board);
            }
            if (step.type === 'revert' && step.cards) {
                for (const card of step.cards) {
                    const cardKey = getCardKey(card);
                    revertingCards.current.delete(cardKey);
                    optimisticCardPositions.current.delete(cardKey);
                }
            }
            if (pendingCompletionCallbackRef.current && remainingSequenceEventsRef.current > 0) {
                remainingSequenceEventsRef.current--;
            }
        },
        onIdle: () => {
            // Allows future legitimate duplicates of a sequence already played.
            if (processedEventContent.current.size > 0) processedEventContent.current.clear();
            if (pendingCompletionCallbackRef.current && remainingSequenceEventsRef.current === 0) {
                const callback = pendingCompletionCallbackRef.current;
                pendingCompletionCallbackRef.current = null;
                callback();
            }
        },
        // A bot's move is a move: the bump timer only nudges a table nobody moved.
        onQueued: (steps) => {
            if (!url_game_id) return;
            const seats = games[url_game_id]?.seats;
            if (!seats) return;
            if (steps.some((e) => e.seat !== undefined && seats[e.seat]?.isAi)) {
                hasBotMovedRef.current = true;
            }
        },
    });

    // Queue a single animation
    const queueAnimation = (event: ClientAnimationEvent) => enqueue([event]);

    // Get animation state for a specific card
    const getCardAnimationState = (card: Card, owner?: number | string) => {
        const cardKey = getCardKeyOwner(card, owner);
        const cardAnimation = animatingCards.get(cardKey);

        if (cardAnimation) {
            return {
                isAnimating: true,
                animationType: cardAnimation.animationType,
                progress: cardAnimation.progress,
                fromLocation: cardAnimation.fromLocation,
                toLocation: cardAnimation.toLocation
            };
        }

        return {
            isAnimating: false,
            animationType: null,
            progress: 0,
            fromLocation: null,
            toLocation: null
        };
    };

    // THE BOARD A PREDICTED MOVE LEAVES, made by the kernel from whatever is on
    // screen at the moment the prediction's flight lands (clientBoards.optimisticBoard:
    // the table, the hand, a pass's shield, a pickup's rotation). It used to be a
    // second ANIMATION_TIME timer in ServerContext, coupled to this flight by
    // nothing but the two files reading the same constant.
    const predictedLanding = (wire: Uint8Array, still: () => boolean) => ({
        commit_board: () => {
            const held = currentGameRef.current;
            return held ? optimisticBoard(held, wire) : null;
        },
        commit_if: still,
    });

    // Helper function to trigger optimistic animation and track it
    const triggerOptimisticAnimation = (animationType: string, cards: Card[], fromLocation: string, toLocation: string, seat?: number, targetCard?: Card, battleIndex?: number, landing?: { commit_board: () => TableView | null; commit_if: () => boolean }) => {
        const animationEvent: ClientAnimationEvent = {
            type: animationType as any,
            cards: cards,
            from_location: fromLocation as any,
            to_location: toLocation as any,
            seat,
            target_card: targetCard,
            battle_index: battleIndex,
            message: `Optimistic ${animationType} animation`,
            ...landing,
        };

        // Track EACH CARD individually to avoid duplicates from server
        // Server may split multi-card actions into separate events (one per card)
        const timestamp = Date.now();
        cards.forEach(card => {
            const cardKey = getCardKey(card);
            rememberPending(animationType, card, fromLocation, toLocation, seat, timestamp);

            // Track visual position for revert animations
            // After animation completes, card will VISUALLY be at toLocation
            const positionInfo: any = {
                location: toLocation,
                seat
            };
            if (targetCard !== undefined) {
                positionInfo.target_card = targetCard;
            }
            if (battleIndex !== undefined) {
                positionInfo.battle_index = battleIndex;
            }
            optimisticCardPositions.current.set(cardKey, positionInfo);
        });

        // Queue the optimistic animation immediately
        queueAnimation(animationEvent);
    };

    // TIMING FLOW:
    // 1. User action validates in AnimationContext (instant rejection if invalid)
    // 2. Optimistic animation triggers immediately (instant feedback)
    // 3. ServerContext puts the board the move leaves on screen after ANIMATION_TIME,
    //    unless the server has refused a card-laying move by then: its cards fly
    //    home from a table that does not show them. (The kernel leaves a board that
    //    already shows the move as it is: a confirmation that landed first.)
    // 4. Server response with intermediate states provides final truth

    // The board a refused move's cards fly home to. While no push has moved the
    // game on - the board on screen is still the version the move was made on - it
    // is that board with the move undone; once a push has, the server's board is on
    // its way through the queue and nothing is made here.
    const refusedBoard = (tap: TableView, undo: (held: TableView) => TableView | null): TableView | undefined => {
        const held = currentGameRef.current;
        if (!held || held.version !== tap.version) return undefined;
        return undo(held) ?? undefined;
    };

    // Game action methods that handle optimistic animations + server calls
    const attack = async (cards: Card[]): Promise<{ game_id: string }> => {
        if (!game_id || !games[game_id]) {
            throw new Error('No active game');
        }

        const game = games[game_id];

        // Build the awire bytes ONCE per move: the exact buffer the kernel
        // validates below is what travels as the binary POST body.
        const wire = encodeAction({ kind: 'attack', cards });

        // 1. Send the request BEFORE validating - the server is authoritative and
        //    rejects illegal moves, so we don't block the round-trip on local
        //    validation. `valid` is captured by the server method's deferred
        //    optimistic patch (applied only if still valid) and gates the optimistic
        //    animation below.
        let valid = true, refused = false;
        const serverPromise = serverActions.attack(cards, wire);

        // 2. Validate the SAME wire bytes locally; only add optimistic feedback
        //    if the move is legal.
        try {
            validateActionWire(game, wire);
        } catch {
            valid = false;
        }
        if (valid) {
            triggerOptimisticAnimation('attack_pass', cards, 'hand', 'table', seatOf(game), undefined, undefined,
                predictedLanding(wire, () => valid && !refused));
        }

        // 3. Await the server's verdict (revert-on-rejection below).
        try {
            return await serverPromise;
        } catch (error) {
            // Server rejected the attack - but check if we already reverted due to conflict detection
            refused = true;
            // The cards land back in my hand, off the table the move may already stand on.
            const homeBoard = refusedBoard(game, (held) => withdrawn(held, cards));

            cards.forEach(card => {
                const cardKey = getCardKey(card);

                // Check if this card is already being reverted OR tracking was cleared
                const isCurrentlyReverting = revertingCards.current.has(cardKey);
                const wasAlreadyReverted = !optimisticCardPositions.current.has(cardKey);

                // Also check if optimistic animation was cleared (conflict detection clears it)
                const optimisticAnimationCleared =
                    !optimisticAnimations.current.has(animEventKey('attack_pass', card, 'hand', 'table', seatOf(game)));

                if (isCurrentlyReverting || wasAlreadyReverted || optimisticAnimationCleared) {
                    return;
                }

                revertingCards.current.add(cardKey);

                // Get where this card currently is visually
                const visualPosition = optimisticCardPositions.current.get(cardKey);
                const fromLocation = visualPosition?.location || 'table';

                const revertEvent: ClientAnimationEvent = {
                    type: 'revert',
                    cards: [card],
                    from_location: fromLocation as any,
                    to_location: 'hand',
                    seat: seatOf(game),
                    is_revert: true,
                    message: 'Attack rejected by server',
                    game_state: homeBoard
                };

                queueAnimation(revertEvent);

                // Clear from optimistic tracking: a refused card is pending nowhere,
                // so a resync (optimisticOverlay) does not lay it again.
                optimisticAnimations.current.delete(animEventKey('attack_pass', card, 'hand', 'table', seatOf(game)));
                optimisticCardPositions.current.delete(cardKey);
            });

            throw error;
        }
    };

    const pass = async (cards: Card[]): Promise<{ game_id: string }> => {
        if (!game_id || !games[game_id]) {
            throw new Error('No active game');
        }

        const game = games[game_id];

        // One awire buffer for the gate + the POST body (see attack).
        const wire = encodeAction({ kind: 'pass', cards });

        // 1. Send the request BEFORE validating (server is authoritative; see attack).
        let valid = true, refused = false;
        const serverPromise = serverActions.pass(cards, wire);

        // 2. Validate the same wire bytes locally; only add optimistic feedback
        //    if the move is legal.
        try {
            validateActionWire(game, wire);
        } catch {
            valid = false;
        }
        if (valid) {
            // Trigger optimistic animation - single animation with all cards going to their spots
            triggerOptimisticAnimation('attack_pass', cards, 'hand', 'table', seatOf(game), undefined, undefined,
                predictedLanding(wire, () => valid && !refused));

            // Keep the pass itself - the wire I sent - so a board it has not been
            // confirmed on can be asked of the kernel instead of patched from two
            // seat numbers read off this one board at this one moment.
            pendingPass.current = { wire, cards, seat: seatOf(game) };
        }

        // 3. Await the server's verdict (revert-on-rejection below).
        try {
            return await serverPromise;
        } catch (error) {
            // Server rejected the pass - the cards go home and the pass is pending
            // nowhere, so no board is asked about it again.
            refused = true;
            pendingPass.current = null;
            // The cards land back in my hand, and the lead and the shield are as they were.
            const homeBoard = refusedBoard(game, (held) => {
                const back = withdrawn(held, cards);
                return back && turnedBoard(back, game.firstAttacker, game.defender);
            });

            // Check if conflict detection already handled these cards
            const cardsNeedingRevert = cards.filter(card =>
                optimisticAnimations.current.has(animEventKey('attack_pass', card, 'hand', 'table', seatOf(game))));

            if (cardsNeedingRevert.length === 0) {
            } else {

                cardsNeedingRevert.forEach(card => {
                    const cardKey = getCardKey(card);
                    if (revertingCards.current.has(cardKey)) return;
                    revertingCards.current.add(cardKey);

                    const visualPosition = optimisticCardPositions.current.get(cardKey);
                    const fromLocation = visualPosition?.location || 'table';

                    queueAnimation({
                        type: 'revert',
                        cards: [card],
                        from_location: fromLocation as any,
                        to_location: 'hand',
                        seat: seatOf(game),
                        is_revert: true,
                        message: 'Pass rejected by server',
                        game_state: homeBoard
                    });

                    optimisticAnimations.current.delete(animEventKey('attack_pass', card, 'hand', 'table', seatOf(game)));
                    optimisticCardPositions.current.delete(cardKey);
                });
            }
            throw error;
        }
    };

    const pickup = async (): Promise<{ game_id: string }> => {
        if (!game_id || !games[game_id]) {
            throw new Error('No active game');
        }

        const game = games[game_id];
        const allTableCards = getTableCards(game);

        // One awire buffer for the gate + the POST body (see attack).
        const wire = encodeAction({ kind: 'pickup' });

        // 1. Send the request BEFORE validating (server is authoritative; see attack).
        //    A refused pickup still takes the table into the hand once its flight
        //    lands: the cards' return flight starts from the hand, which hides them
        //    while they fly, and the refusal's board puts the table back.
        let valid = true;
        const serverPromise = serverActions.pickup(wire);

        // 2. Validate the same wire bytes locally; only add optimistic feedback
        //    if the move is legal.
        try {
            validateActionWire(game, wire);
        } catch {
            valid = false;
        }
        if (valid) {
            triggerOptimisticAnimation('pickup', allTableCards, 'table', 'hand', seatOf(game), undefined, undefined,
                predictedLanding(wire, () => valid));
        }

        // 3. Await the server's verdict (revert-on-rejection below).
        try {
            return await serverPromise;
        } catch (error) {
            // Server rejected the pickup
            // Check if conflict detection already handled reverts
            const stillTracking = allTableCards.filter(card =>
                optimisticAnimations.current.has(animEventKey('pickup', card, 'table', 'hand', seatOf(game))));

            if (stillTracking.length === 0) {
                throw error;
            }
            // The table goes back to the board the pickup was made on, as long as no push has moved the game on.
            const homeBoard = refusedBoard(game, () => game);

            allTableCards.forEach(card => {
                const cardKey = getCardKey(card);
                if (revertingCards.current.has(cardKey)) return;
                revertingCards.current.add(cardKey);

                const visualPosition = optimisticCardPositions.current.get(cardKey);
                const fromLocation = visualPosition?.location || 'hand';
                const toLocation = fromLocation === 'hand' ? 'table' : 'hand';

                queueAnimation({
                    type: 'revert',
                    cards: [card],
                    from_location: fromLocation as any,
                    to_location: toLocation as any,
                    seat: seatOf(game),
                    is_revert: true,
                    message: 'Pickup rejected by server',
                    game_state: homeBoard
                });

                optimisticAnimations.current.delete(animEventKey('pickup', card, 'table', 'hand', seatOf(game)));
                optimisticCardPositions.current.delete(cardKey);
            });
            throw error;
        }
    };

    const cover = async (coverCards: Card[], attackCards: Card[]): Promise<{ game_id: string }> => {
        if (!game_id || !games[game_id]) {
            throw new Error('No active game');
        }

        const game = games[game_id];

        // One awire buffer for the gate + the POST body (see attack). A
        // mismatched cover/attack pairing throws here (a client bug, not a
        // race) - the caller sees a rejected promise, same as a server reject.
        const wire = encodeAction({ kind: 'cover', cards: coverCards, attack_cards: attackCards });

        // 1. Send the request BEFORE validating (server is authoritative; see attack).
        let valid = true, refused = false;
        const serverPromise = serverActions.cover(coverCards, attackCards, wire);

        // 2. Validate the same wire bytes locally; only add optimistic feedback
        //    if the move is legal.
        try {
            validateActionWire(game, wire);
        } catch {
            valid = false;
        }

        if (valid) {
            // Trigger optimistic cover animation - SINGLE animation with all cards going to their targets
            // Track cover cards with their target attack cards for proper merging later
            const animationEvent: ClientAnimationEvent = {
                type: 'cover',
                cards: coverCards,
                target_cards: attackCards, // Pass the target attack cards for each cover card
                from_location: 'hand',
                to_location: 'table',
                seat: seatOf(game),
                message: 'Optimistic cover animation',
                ...predictedLanding(wire, () => valid && !refused),
            };

            // Track EACH CARD individually with its target attack card
            const timestamp = Date.now();
            coverCards.forEach((coverCard, idx) => {
                const attackCard = attackCards[idx];
                const cardKey = getCardKey(coverCard);

                // Find battle index for this attack
                const battleIndex = game.battles.findIndex(b =>
                    b.attack.suit === attackCard.suit && b.attack.value === attackCard.value
                );

                // Track for conflict detection
                rememberPending('cover', coverCard, 'hand', 'table', seatOf(game), timestamp);

                // Track visual position with target card info for animations
                const positionInfo: any = {
                    location: 'table',
                    seat: seatOf(game),
                    target_card: attackCard,
                    battle_index: battleIndex
                };
                optimisticCardPositions.current.set(cardKey, positionInfo);
            });

            // Queue the single animation with all cover cards
            queueAnimation(animationEvent);
        }

        // 3. Await the server's verdict (revert-on-rejection below).
        try {
            return await serverPromise;
        } catch (error) {
            // Server rejected the cover - create revert animation
            refused = true;
            // The covers land back in my hand, and their attacks stand uncovered.
            const homeBoard = refusedBoard(game, (held) => withdrawn(held, coverCards));
            coverCards.forEach(card => {
                const cardKey = getCardKey(card);
                if (revertingCards.current.has(cardKey)) return;
                revertingCards.current.add(cardKey);

                const visualPosition = optimisticCardPositions.current.get(cardKey);
                const fromLocation = visualPosition?.location || 'table';

                queueAnimation({
                    type: 'revert',
                    cards: [card],
                    from_location: fromLocation as any,
                    to_location: 'hand',
                    seat: seatOf(game),
                    is_revert: true,
                    message: 'Cover rejected by server',
                    game_state: homeBoard
                });

                optimisticAnimations.current.delete(animEventKey('cover', card, 'hand', 'table', seatOf(game)));
                optimisticCardPositions.current.delete(cardKey);
            });
            throw error;
        }
    };

    const good = async (): Promise<{ game_id: string }> => await serverActions.good();

    // The frame loop drops itself on unmount; the bump timer is this file's.
    useEffect(() => {
        return () => {
            if (botBumpTimerRef.current) {
                clearInterval(botBumpTimerRef.current);
            }
        };
    }, []);

    const resetAnimations = useCallback(() => {
        pendingCompletionCallbackRef.current = null;
        remainingSequenceEventsRef.current = 0;
        processedEventContent.current.clear();
        resetRun();
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    return (
        <AnimationContext.Provider value={{
            isAnimating,
            currentAnimation,
            flightMs,
            inFlightFromDeck,
            inFlightToFlipped,
            getCardAnimationState,
            attack,
            pass,
            pickup,
            cover,
            good,
            resetAnimations
        }}>
            {children}
        </AnimationContext.Provider>
    );
};

export const useAnimation = () => {
    const context = useContext(AnimationContext);
    if (!context) {
        throw new Error('useAnimation must be used within an AnimationProvider');
    }
    return context;
};
 