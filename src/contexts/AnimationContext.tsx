import React, { createContext, useContext, useEffect, useState, useRef, useCallback } from 'react';
import { useServer, useServerActions } from './ServerContext';
import { useAuth } from './AuthContext';
import { useParams } from 'next/navigation';
import supabase from '../backend/Connector';
import { ANIMATION_TIME } from '../constants/constants';
import { validateActionWire } from '../utils/gameValidation';
import { encodeAction } from '@sdk/ts/wire/awire.ts';
import { clientTable } from '@sdk/ts/table/client_table.ts';
import { pushToSequence } from '../state/pushSequence';
import { covered, rulesOf, type TableView, type ViewCard } from '../state/view';
import { keepPending, lifted, optimisticBoard, returnedToHand, tableOf, turnedBoard, withdrawn } from '../state/clientBoards';
import { base64ToBytes } from '@sdk/ts/wire/bytes.ts';
import { getTableCards, cardsIntersection, getCardKeyOwner, createCardEventString, getCardKey } from '../utils/animationUtils';
import { animationFeed } from '../state/animationFeed';
import { staleOptimisticKeysOnTable } from '../state/optimisticAnimation';
import { resolveUnconfirmedAttackCovers, resolveConflictMotions, CONFLICT_DEST } from '../state/optimisticConflicts';
import { optimisticOverlay } from '../state/optimisticOverlay';
import { shouldDropStaleSequence } from '../state/clientReconcile';
import { noteAuthoritativeVersion } from '../state/authoritativeVersion';

// Animation timing constant
export { ANIMATION_TIME } from '../constants/constants';

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
}

interface AnimationContextType {
    isAnimating: boolean;
    currentAnimation: ClientAnimationEvent | null;
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

    const [isAnimating, setIsAnimating] = useState(false);
    const [currentAnimation, setCurrentAnimation] = useState<ClientAnimationEvent | null>(null);
    const [animationQueue, setAnimationQueue] = useState<ClientAnimationEvent[]>([]);
    const [inFlightFromDeck, setInFlightFromDeck] = useState(0);
    const [inFlightToFlipped, setInFlightToFlipped] = useState(0);

    const [animatingCards, setAnimatingCards] = useState<Map<string, {
        animationType: string;
        progress: number;
        fromLocation: string | null;
        toLocation: string | null;
        startTime: number;
    }>>(new Map());

    const timeoutRef = useRef<NodeJS.Timeout | null>(null);
    // The queue itself. The state is its copy for rendering (it starts the queue);
    // the ref is read and changed synchronously, so a flight that lands can start
    // the next one in the same commit and an event queued before a render is never
    // missed.
    const animationQueueRef = useRef<ClientAnimationEvent[]>([]);
    const enqueue = (events: ClientAnimationEvent[]) => {
        if (events.length === 0) return;
        animationQueueRef.current = [...animationQueueRef.current, ...events];
        setAnimationQueue(animationQueueRef.current);
    };
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

    // Track optimistically triggered animations to avoid server duplicates
    // Map of animation hash -> timestamp when it was added
    const optimisticAnimations = useRef<Map<string, number>>(new Map());

    // Track cards that are currently being reverted to avoid duplicate revert animations
    const revertingCards = useRef<Set<string>>(new Set());

    // Track visual positions of optimistically animated cards (for accurate revert animations)
    // Map of cardKey -> { location: 'table' | 'hand', seat, target_card?: Card, battle_index?: number }
    const optimisticCardPositions = useRef<Map<string, { location: string, seat?: number, target_card?: Card, battle_index?: number }>>(new Map());

    // Track optimistic pass state (defender and first_attacker changes)
    const optimisticPassState = useRef<{ defender: number, first_attacker: number } | null>(null);

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
            const toDelete: string[] = [];
            optimisticAnimations.current.forEach((timestamp, hash) => {
                if (now - timestamp > threshold) {
                    toDelete.push(hash);
                }
            });

            toDelete.forEach(hash => {
                optimisticAnimations.current.delete(hash);
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

                const cardEventString = createCardEventString('attack_pass', optCard, 'hand', 'table', mySeat);
                optimisticAnimations.current.delete(cardEventString);
            });
        };

        optimisticAnimations.current.forEach((timestamp, cardEventString) => {
            try {
                const parsedEvent = JSON.parse(cardEventString);
                if (parsedEvent.seat === mySeat) {
                    // Attacks and covers (hand → table)
                    if ((parsedEvent.type === 'attack_pass' || parsedEvent.type === 'cover') &&
                        parsedEvent.from_location === 'hand' &&
                        parsedEvent.to_location === 'table') {
                        myOptimisticAttackCovers.push(parsedEvent.card);
                        if (parsedEvent.type === 'cover') {
                            myOptimisticCoverKeys.add(getCardKey(parsedEvent.card));
                        }
                    }
                    // Pickups (table → hand)
                    else if (parsedEvent.type === 'pickup' &&
                        parsedEvent.from_location === 'table' &&
                        parsedEvent.to_location === 'hand') {
                        myOptimisticPickups.push(parsedEvent.card);
                    }
                }
            } catch (e) {
                // Skip invalid entries
            }
        });

        // Check if server's final state already includes my optimistic attack/cover cards
        // If so, server accepted them - don't revert!
        const myOptimisticCardsAccepted = cardsIntersection(myOptimisticAttackCovers, serverTableCards);

        const serverAttackPasses = message.events.filter((evt: any) => evt.type === 'attack_pass');

        // ====== CHECK FOR OPTIMISTIC PASS CONFLICTS EARLY ======
        // Do this BEFORE merging, so invalid pass cards don't get baked into states
        if (optimisticPassState.current && message.events.length > 0 && serverAttackPasses.length > 0) {
            const nextDefenderId = optimisticPassState.current.defender;
            const finalGameState: TableView = message.game || serverState;

            // My still-pending pass cards.
            const passCards: Card[] = [];
            optimisticAnimations.current.forEach((timestamp, key) => {
                try {
                    const parsed = JSON.parse(key);
                    if (parsed.type === 'attack_pass' && parsed.seat === mySeat
                        && optimisticCardPositions.current.has(getCardKey(parsed.card))) {
                        passCards.push(parsed.card);
                    }
                } catch (e) { }
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

                    // Clear optimistic pass state and card tracking
                    optimisticPassState.current = null;

                    passCardsToRevert.forEach(card => {
                        const cardEventString = createCardEventString('attack_pass', card, 'hand', 'table', mySeat);
                        optimisticAnimations.current.delete(cardEventString);
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
                    const cardEventString = createCardEventString('pickup', card, 'table', 'hand', mySeat);
                    optimisticAnimations.current.delete(cardEventString);
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
                optimisticAnimations.current.delete(createCardEventString('attack_pass', card, 'hand', 'table', mySeat));
                optimisticAnimations.current.delete(createCardEventString('cover', card, 'hand', 'table', mySeat));
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
                    const cardEventString = createCardEventString('attack_pass', card, 'hand', 'table', mySeat);
                    optimisticAnimations.current.delete(cardEventString);
                });
            }

            if (cardsToMerge.length > 0) {
                // Check if any pass events in this message are my own
                const hasUserPass = message.events.some((evt: any) =>
                    evt.type === 'attack_pass' && evt.seat !== undefined && evt.seat === mySeat
                );
                // Each card with the attack it covers, if it is a cover.
                const pending = cardsToMerge.map((card: Card) => ({
                    card,
                    target: optimisticCardPositions.current.get(getCardKey(card))?.target_card ?? null,
                }));

                // Keep the optimistic cards on ALL boards (events + final): the kernel lays
                // each one the board does not already show, over its target or as an
                // attack, and takes it out of my hand. FIRST, the lead and the shield of my
                // pending pass, if this message contains it; otherwise the server's board
                // is right (another player passed).
                const keep = (board: TableView): TableView => {
                    let next: TableView | null = board;
                    if (optimisticPassState.current && hasUserPass) {
                        next = turnedBoard(next, optimisticPassState.current.first_attacker, optimisticPassState.current.defender);
                    }
                    return (next && keepPending(next, pending)) ?? board;
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
                for (const key of staleOptimisticKeysOnTable(optimisticAnimations.current.keys(), tableCards, message.events)) {
                    optimisticAnimations.current.delete(key);
                    try {
                        optimisticCardPositions.current.delete(getCardKey(JSON.parse(key).card));
                    } catch { /* ignore malformed key */ }
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

            const allCardsOptimistic = serverEvent.cards.every((card: Card) => {
                const cardEventString = createCardEventString(serverEvent.type, card, serverEvent.from_location, serverEvent.to_location, serverEvent.seat);
                const timestamp = optimisticAnimations.current.get(cardEventString);
                const isOptimistic = timestamp !== undefined;
                return isOptimistic;
            });

            if (allCardsOptimistic) {
                optimisticEventIndices.push(eventIndex);
                // Clear the optimistic animations since server confirmed them
                serverEvent.cards.forEach((card: Card) => {
                    const cardKey = getCardKey(card);
                    const cardEventString = createCardEventString(serverEvent.type, card, serverEvent.from_location, serverEvent.to_location, serverEvent.seat);
                    optimisticAnimations.current.delete(cardEventString);

                    // Also clear position tracking since server confirmed the move
                    optimisticCardPositions.current.delete(cardKey);

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
            
            // Check if any pass events in this message are my own
            const hasUserPass = message.events.some((evt: any) =>
                evt.type === 'attack_pass' && evt.seat !== undefined && evt.seat === message.game.mySeat
            );
            
            // Only apply optimistic pass state if this message contains a pass from the current user
            // Otherwise, trust the server's game state (which is correct for other players' passes)
            if (optimisticPassState.current && hasUserPass) {
                // Check if server confirmed optimistic pass
                const serverConfirmedPass =
                    message.game.defender === optimisticPassState.current.defender &&
                    message.game.firstAttacker === optimisticPassState.current.first_attacker;

                if (serverConfirmedPass) {
                    optimisticPassState.current = null;
                } else {
                    // Server didn't confirm - use optimistic state (our pass might have been rejected or modified)
                    message.game = turnedBoard(message.game, optimisticPassState.current.first_attacker, optimisticPassState.current.defender) ?? message.game;
                }
            } else if (optimisticPassState.current && !hasUserPass) {
                // This message doesn't contain our pass, so clear stale optimistic state
                // and trust the server (another player passed)
                optimisticPassState.current = null;
            }

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

        // Find the first attack event from server (the valid attack)
        const firstAttackIndex = message.events.findIndex((evt: any) =>
            evt.type === 'attack_pass' && evt.from_location === 'hand'
        );

        // Find any pickup/clear events
        const firstPickupIndex = message.events.findIndex((evt: any) =>
            evt.type === 'pickup' || evt.type === 'cards_to_trash'
        );

        // Find magic_transition (for good scenario with pickup reverts)
        const firstMagicTransitionIndex = message.events.findIndex((evt: any) =>
            evt.type === 'magic_transition'
        );

        // Check if we have pickup reverts (hand → table)
        const hasPickupReverts = revertEvents.some(rev => rev.to_location === 'table');

        if (hasPickupReverts && firstMagicTransitionIndex >= 0) {
            // For optimistic pickup + server good: revert ALL cards back to table first
            const eventsBeforeMagic = message.events.slice(0, firstMagicTransitionIndex);
            const restEvents = message.events.slice(firstMagicTransitionIndex);

            const queueOrder = [
                ...eventsBeforeMagic,
                ...revertEvents,    // Revert pickups back to table first
                ...restEvents       // Then magic_transition + cards_to_trash
            ];

            enqueue(queueOrder);
        } else if (firstAttackIndex >= 0) {
            // Queue reverts IMMEDIATELY before the valid attack for parallel visual effect
            const eventsBeforeAttack = message.events.slice(0, firstAttackIndex);
            const restEvents = message.events.slice(firstAttackIndex);

            const queueOrder = [
                ...eventsBeforeAttack,
                ...revertEvents,    // Revert animates
                ...restEvents       // Valid attack animates right after (looks parallel)
            ];

            enqueue(queueOrder);
        } else if (firstPickupIndex >= 0) {
            // Queue reverts before the pickup so card goes back to hand first
            const eventsBeforePickup = message.events.slice(0, firstPickupIndex);
            const restEvents = message.events.slice(firstPickupIndex);

            const queueOrder = [
                ...eventsBeforePickup,
                ...revertEvents,    // Revert animates first
                ...restEvents       // Then pickup animates
            ];

            enqueue(queueOrder);
        } else {
            enqueue([...revertEvents, ...message.events]);
        }
    };


    // TODO(redo properly): this serial setTimeout-driven event queue is a React
    // workaround for the lack of a shared-element transition. It should be replaced
    // with a proper animation model (the way the iOS client does it - GPU-driven
    // matchedGeometry-style flights + structured sequencing, no setTimeout chain).
    // Process the animation queue
    const processAnimationQueue = useCallback(() => {
        if (animationQueueRef.current.length === 0) {
            setIsAnimating(false);
            setCurrentAnimation(null);
            setInFlightFromDeck(0);
            setInFlightToFlipped(0);

            // Clear processed event content when queue is empty (allows future legitimate duplicates)
            if (processedEventContent.current.size > 0) {
                processedEventContent.current.clear();
            }

            // Check if we have a pending completion callback and we've finished the sequence
            if (pendingCompletionCallbackRef.current && remainingSequenceEventsRef.current === 0) {
                const callback = pendingCompletionCallbackRef.current;
                pendingCompletionCallbackRef.current = null;
                remainingSequenceEventsRef.current = 0;
                callback();
            }

            return;
        }

        const nextAnimation = animationQueueRef.current[0];
        animationQueueRef.current = animationQueueRef.current.slice(1);

        // Check if this animation is from a bot player
        if (nextAnimation.seat !== undefined && url_game_id) {
            const currentGame = games[url_game_id];
            const seat = currentGame?.seats[nextAnimation.seat];
            if (seat?.isAi) {
                // This is a bot move - set the flag
                hasBotMovedRef.current = true;
            }
        }

        setCurrentAnimation(nextAnimation);
        setAnimationQueue(animationQueueRef.current);
        setIsAnimating(true);

        // Drop the deck's displayed count NOW (in the same render as currentAnimation
        // becomes visible) so the deck shrinks in lockstep with the cards leaving.
        // Cards bound for the flipped slot stay in the deck system (they don't
        // affect the badge total), so we track them separately.
        if (nextAnimation.from_location === 'deck' && nextAnimation.cards && nextAnimation.cards.length > 0) {
            setInFlightFromDeck(nextAnimation.cards.length);
            setInFlightToFlipped(nextAnimation.to_location === 'flipped' ? nextAnimation.cards.length : 0);
        } else {
            setInFlightFromDeck(0);
            setInFlightToFlipped(0);
        }

        // The cards in flight, hidden at the places the flight leaves and lands on
        // (flightPlaces) until it lands.
        const places = flightPlaces(nextAnimation.from_location, nextAnimation.to_location, nextAnimation.seat);
        if (nextAnimation.cards && nextAnimation.cards.length > 0) {
            setAnimatingCards(prev => {
                const newAnimatingCards = new Map(prev);

                nextAnimation.cards!.forEach(card => places.forEach(place => {
                    const cardKey = getCardKeyOwner(card, place);
                    newAnimatingCards.set(cardKey, {
                        animationType: nextAnimation.type,
                        progress: 1, // Always 1 - CSS transitions handle the animation
                        fromLocation: nextAnimation.from_location || null,
                        toLocation: nextAnimation.to_location || null,
                        startTime: Date.now()
                    });
                }));

                return newAnimatingCards;
            });
        }

        // Animation duration: use ANIMATION_TIME constant for consistency
        timeoutRef.current = setTimeout(() => {
            // UPDATE THE GAME STATE WITH THE INTERMEDIATE STATE AFTER ANIMATION COMPLETES
            // The game: the last push's, or - for a refused move's board before any
            // push has played - the board's own.
            const commitGameId = currentGameIdRef.current ?? nextAnimation.game_state?.gameId;
            if (nextAnimation.game_state && commitGameId) {
                let board = nextAnimation.game_state;

                // If we have an optimistic pass, preserve defender/first_attacker
                if (optimisticPassState.current) {
                    board = turnedBoard(board, optimisticPassState.current.first_attacker, optimisticPassState.current.defender) ?? board;
                }

                updateGameState(commitGameId, board);
            }

            // Cards have landed; game.deck_length now reflects the reduction, so
            // clear the in-flight counts to avoid double-counting during the gap.
            setInFlightFromDeck(0);
            setInFlightToFlipped(0);

            // Remove cards from animating state
            if (nextAnimation.cards) {
                setAnimatingCards(prev => {
                    const updated = new Map(prev);
                    nextAnimation.cards!.forEach(card => places.forEach(place => {
                        updated.delete(getCardKeyOwner(card, place));
                    }));
                    return updated;
                });

                // If this was a revert animation, clear the reverting and position tracking
                if (nextAnimation.type === 'revert') {
                    nextAnimation.cards.forEach(card => {
                        const cardKey = getCardKey(card);
                        revertingCards.current.delete(cardKey);
                        optimisticCardPositions.current.delete(cardKey);
                    });
                }
            }

            // Decrement remaining sequence events count if we're tracking a sequence
            if (pendingCompletionCallbackRef.current && remainingSequenceEventsRef.current > 0) {
                remainingSequenceEventsRef.current--;
            }

            // The next flight starts in this same commit: the board this one landed
            // on, its card shown where it landed, and the next flight's cards
            // (AnimationOverlay builds them before the browser paints) are one frame,
            // so no card is ever drawn in two places or in none between two events.
            processAnimationQueueRef.current();
        }, ANIMATION_TIME);
    }, [updateGameState, url_game_id, games]);
    const processAnimationQueueRef = useRef(processAnimationQueue);
    processAnimationQueueRef.current = processAnimationQueue;

    // Start processing queue when items are added and no animation is running
    useEffect(() => {
        if (animationQueueRef.current.length > 0 && !isAnimating) {
            processAnimationQueue();
        }
    }, [animationQueue, isAnimating, processAnimationQueue]);

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

    // Helper function to trigger optimistic animation and track it
    const triggerOptimisticAnimation = (animationType: string, cards: Card[], fromLocation: string, toLocation: string, seat?: number, targetCard?: Card, battleIndex?: number) => {
        const animationEvent: ClientAnimationEvent = {
            type: animationType as any,
            cards: cards,
            from_location: fromLocation as any,
            to_location: toLocation as any,
            seat,
            target_card: targetCard,
            battle_index: battleIndex,
            message: `Optimistic ${animationType} animation`
        };

        // Track EACH CARD individually to avoid duplicates from server
        // Server may split multi-card actions into separate events (one per card)
        const timestamp = Date.now();
        cards.forEach(card => {
            const cardKey = getCardKey(card);
            const cardEventString = createCardEventString(animationType, card, fromLocation, toLocation, seat);
            optimisticAnimations.current.set(cardEventString, timestamp);

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
        const serverPromise = serverActions.attack(cards, () => valid && !refused, wire);

        // 2. Validate the SAME wire bytes locally; only add optimistic feedback
        //    if the move is legal.
        try {
            validateActionWire(game, wire);
        } catch {
            valid = false;
        }
        if (valid) {
            triggerOptimisticAnimation('attack_pass', cards, 'hand', 'table', seatOf(game));
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
                const attackCardEventString = createCardEventString('attack_pass', card, 'hand', 'table', seatOf(game));
                const optimisticAnimationCleared = !optimisticAnimations.current.has(attackCardEventString);

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
                const fallbackCardEventString = createCardEventString('attack_pass', card, 'hand', 'table', seatOf(game));
                optimisticAnimations.current.delete(fallbackCardEventString);
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
        const serverPromise = serverActions.pass(cards, () => valid && !refused, wire);

        // 2. Validate the same wire bytes locally; only add optimistic feedback
        //    if the move is legal.
        try {
            validateActionWire(game, wire);
        } catch {
            valid = false;
        }
        if (valid) {
            // Trigger optimistic animation - single animation with all cards going to their spots
            triggerOptimisticAnimation('attack_pass', cards, 'hand', 'table', seatOf(game));

            // Track optimistic pass state (defender will change to next player).
            // Pass moves defender to the next IN-PLAY player (skipping eliminated
            // seats, exactly like the server's get_next_player_index): the shield
            // the kernel's optimistic board for this pass gives. first_attacker
            // does NOT change during a pass (only changes on new round).
            const passed = optimisticBoard(game, wire);
            if (passed) {
                optimisticPassState.current = {
                    defender: passed.defender,
                    first_attacker: game.firstAttacker  // Unchanged
                };
            }
        }

        // 3. Await the server's verdict (revert-on-rejection below).
        try {
            return await serverPromise;
        } catch (error) {
            // Server rejected the pass - clear optimistic pass state and create revert animation (if not already handled)
            refused = true;
            optimisticPassState.current = null;
            // The cards land back in my hand, and the lead and the shield are as they were.
            const homeBoard = refusedBoard(game, (held) => {
                const back = withdrawn(held, cards);
                return back && turnedBoard(back, game.firstAttacker, game.defender);
            });

            // Check if conflict detection already handled these cards
            const cardsNeedingRevert = cards.filter(card => {
                const cardEventString = createCardEventString('attack_pass', card, 'hand', 'table', seatOf(game));
                return optimisticAnimations.current.has(cardEventString);
            });

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

                    optimisticAnimations.current.delete(createCardEventString('attack_pass', card, 'hand', 'table', seatOf(game)));
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
        const serverPromise = serverActions.pickup(() => valid, wire);

        // 2. Validate the same wire bytes locally; only add optimistic feedback
        //    if the move is legal.
        try {
            validateActionWire(game, wire);
        } catch {
            valid = false;
        }
        if (valid) {
            triggerOptimisticAnimation('pickup', allTableCards, 'table', 'hand', seatOf(game));
        }

        // 3. Await the server's verdict (revert-on-rejection below).
        try {
            return await serverPromise;
        } catch (error) {
            // Server rejected the pickup
            // Check if conflict detection already handled reverts
            const stillTracking = allTableCards.filter(card => {
                const cardEventString = createCardEventString('pickup', card, 'table', 'hand', seatOf(game));
                return optimisticAnimations.current.has(cardEventString);
            });

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

                optimisticAnimations.current.delete(createCardEventString('pickup', card, 'table', 'hand', seatOf(game)));
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
        const serverPromise = serverActions.cover(coverCards, attackCards, () => valid && !refused, wire);

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
                message: 'Optimistic cover animation'
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
                const cardEventString = createCardEventString('cover', coverCard, 'hand', 'table', seatOf(game));
                optimisticAnimations.current.set(cardEventString, timestamp);

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

                optimisticAnimations.current.delete(createCardEventString('cover', card, 'hand', 'table', seatOf(game)));
                optimisticCardPositions.current.delete(cardKey);
            });
            throw error;
        }
    };

    const good = async (): Promise<{ game_id: string }> => await serverActions.good();

    // Cleanup timeouts on unmount
    useEffect(() => {
        return () => {
            if (timeoutRef.current) {
                clearTimeout(timeoutRef.current);
            }
            if (botBumpTimerRef.current) {
                clearInterval(botBumpTimerRef.current);
            }
        };
    }, []);

    const resetAnimations = useCallback(() => {
        if (timeoutRef.current) {
            clearTimeout(timeoutRef.current);
            timeoutRef.current = null;
        }
        pendingCompletionCallbackRef.current = null;
        remainingSequenceEventsRef.current = 0;
        animationQueueRef.current = [];
        processedEventContent.current.clear();
        setAnimationQueue([]);
        setCurrentAnimation(null);
        setIsAnimating(false);
        setInFlightFromDeck(0);
        setInFlightToFlipped(0);
        setAnimatingCards(new Map());
    }, []);

    return (
        <AnimationContext.Provider value={{
            isAnimating,
            currentAnimation,
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
 