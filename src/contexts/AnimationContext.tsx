import React, { createContext, useContext, useEffect, useState, useRef, useCallback } from 'react';
import { useServer, useServerActions } from './ServerContext';
import { useAuth } from './AuthContext';
import { useParams } from 'next/navigation';
import supabase from '../backend/Connector';
import { validateActionWire } from '../utils/gameValidation';
import { encodeAction } from '@sdk/ts/wire/awire.ts';
import { clientTable } from '@sdk/ts/table/client_table.ts';
import { pushToSequence } from '../state/pushSequence';
import { rulesOf, tableCards, type TableView, type ViewCard } from '../state/view';
import { optimisticBoard, turnedBoard, withdrawn } from '../state/clientBoards';
import { base64ToBytes } from '@sdk/ts/wire/bytes.ts';
import { getCardKeyOwner, getCardKey } from '../utils/animationUtils';
import { animationFeed } from '../state/animationFeed';
import { revertsFirst } from '../state/revertFlights';
import type { ClientAnimationEvent } from '../state/animationStep';
import {
    passedBoard, pendingOverlayCards, releaseConfirmedOnTable, rememberPending, resolveOptimisticConflicts, seatOf,
    sweepStaleMotions, withoutConfirmedMotions,
    type OptimisticState,
} from '../state/optimisticResolve';
import { optimisticOverlay } from '../state/optimisticOverlay';
import { shouldDropStaleSequence } from '../state/clientReconcile';
import { noteAuthoritativeVersion } from '../state/authoritativeVersion';
import { animEventKey } from '@sdk/ts/wasm/bots.ts';
import { useAnimationRun } from '../state/useAnimationRun';

// Bot bump timeout - 20 seconds of no animations (currently unused)
// const BOT_BUMP_TIMEOUT = 20000;

type Card = ViewCard;

// Every board this pipeline commits is a board it was given - a push's step, a
// replay frame - or one the kernel made from one (src/state/clientBoards.ts): my
// pending cards kept on a push's boards, the lead and shield of my pending pass,
// the board a revert flies home to. Nothing here edits a board.

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

interface AnimationContextType {
    isAnimating: boolean;
    currentAnimation: ClientAnimationEvent | null;
    /** How long the flight on screen lasts, from the kernel's plan: the number
     *  the overlay's CSS transition is written with. 0 when nothing is flying. */
    flightMs: number;
    /** How long the BATTLE ROW's own layout change lasts - the plan's duration
     *  for the landing that moved it. Not `flightMs`: a landing falls in the gap
     *  between two flights, where that one is already 0. 0 before a run's first
     *  landing and after a seek, which are both meant to be instant. */
    rowMs: number;
    /** THE PILES THE GRID MUST NOT MAKE ROOM FOR YET - the cards this run is
     *  still carrying to the table, which get no cell until they land
     *  (src/state/animPlan.ts heldPiles, drawn through `shownRow`). Empty on
     *  every frame with nothing in the air, and the same object each time, so
     *  it compares by identity. */
    heldPiles: ReadonlySet<number>;
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

    // MY MOVES THE SERVER HAS NOT ANSWERED: the predictions on screen, where each
    // one is drawn, the cards a revert is already carrying home, and my pending
    // pass. src/state/optimisticResolve.ts owns the shape and every routine that
    // settles it; this file only holds it and hands it over.
    //
    // ONE ref, not four, because they are one thing and every routine there needs
    // more than one of them. The three collections are mutated in place and never
    // replaced; the pass IS replaced, so it lives in its own cell inside - which
    // is why it reads `optimistic.pass.current` while the rest do not.
    const optimistic = useRef<OptimisticState>({
        motions: new Map(),
        reverting: new Set(),
        positions: new Map(),
        pass: { current: null },
    }).current;

    // Expose the local player's live optimistic table cards to the REST load path,
    // so a reconnect resync re-applies them instead of momentarily wiping them
    // (the "vanish then reappear" glitch). Derived on demand from the live
    // position tracking, so it's always current.
    useEffect(() => optimisticOverlay.register(() => pendingOverlayCards(optimistic)), []);

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
        const interval = setInterval(() => sweepStaleMotions(optimistic, Date.now(), 30000), 5000);
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

            // My cards this AUTHORITATIVE state shows on the table but whose own
            // confirming broadcast the gate dropped. message.game is still the
            // pristine server board here - resolveOptimisticConflicts has not
            // kept anything on it - which is what the release is judged against.
            releaseConfirmedOnTable(optimistic, message);
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

        // Only the events this client has not already animated as a prediction;
        // the ones it has release their tracking there.
        const nonOptimisticEvents = withoutConfirmedMotions(optimistic, message.events);

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

        // CONFLICT DETECTION: which of my unanswered moves this push dooms, and
        // the flights that carry them home (src/state/optimisticResolve.ts).
        const { revertEvents, passIsInvalid } = resolveOptimisticConflicts(optimistic, message);

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
            message.game = passedBoard(optimistic, message.game) ?? message.game;

            updateGameState(message.game.gameId, message.game);
        };
        remainingSequenceEventsRef.current = message.events.length + revertEvents.length;

        // Nothing doomed: the push plays as it came. Otherwise every return
        // flight goes first, over the board it must be drawn against
        // (src/state/revertFlights.ts).
        enqueue(revertEvents.length === 0
            ? message.events
            : revertsFirst(message, revertEvents, passIsInvalid));
    };


    // ONE FRAME LOOP, and the kernel answers it (src/state/useAnimationRun.ts).
    // What is left here is what a landing MEANS - which board it commits, what
    // tracking it releases, when a sequence's final board is the truth - because
    // that is about the game, and the loop is about time.
    const {
        isAnimating, currentAnimation, flightMs, rowMs, heldPiles,
        inFlightFromDeck, inFlightToFlipped, animatingCards, enqueue, reset: resetRun,
    } = useAnimationRun<ClientAnimationEvent>({
        board: () => currentGameRef.current,
        placesOf: (step) => flightPlaces(step.from_location, step.to_location, step.seat),
        keyOf: (card, place) => getCardKeyOwner(card, place),
        // ONE MOVE, ONE MOVEMENT. The kernel spends one COVER event per card, so
        // a defender who covered two attacks in one move arrives as two steps
        // and the plan opens both at the same instant (AnimPlanStep.beat_n).
        // This is what the page draws for that instant: the steps' cards in one
        // flight, each still aimed at the pile the kernel named it for -
        // `target_cards` is the per-card form the overlay already reads for a
        // multi-card cover of my own, and it is the only form that survives the
        // merge, because `target_card` and `battle_index` describe ONE event.
        // The board is the LAST step's: the boards inside one move are boards
        // nobody was ever shown.
        //
        // RENDERING ONLY. Every step of the beat still has its own landing
        // taken, in order, so `commit_board`, `commit_if` and the sequence
        // countdown are untouched by this.
        mergeBeat: (steps) => {
            const cards = steps.flatMap((s) => s.cards ?? []);
            const targets = steps.flatMap((s) => (s.cards ?? []).map(() => s.target_card));
            const named = targets.filter((t): t is Card => t !== undefined);
            return {
                ...steps[0],
                cards,
                // All or nothing: a half-named list would slide every card after
                // the gap onto the wrong pile, where no list at all leaves the
                // overlay's own last-resort guess exactly as it was.
                target_cards: named.length === cards.length ? named : undefined,
                battle_index: undefined,
                game_state: steps[steps.length - 1].game_state ?? steps[0].game_state,
            };
        },
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
                    optimistic.reverting.delete(cardKey);
                    optimistic.positions.delete(cardKey);
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
            rememberPending(optimistic, animationType, card, fromLocation, toLocation, seat, timestamp);

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
            optimistic.positions.set(cardKey, positionInfo);
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
                const isCurrentlyReverting = optimistic.reverting.has(cardKey);
                const wasAlreadyReverted = !optimistic.positions.has(cardKey);

                // Also check if optimistic animation was cleared (conflict detection clears it)
                const optimisticAnimationCleared =
                    !optimistic.motions.has(animEventKey('attack_pass', card, 'hand', 'table', seatOf(game)));

                if (isCurrentlyReverting || wasAlreadyReverted || optimisticAnimationCleared) {
                    return;
                }

                optimistic.reverting.add(cardKey);

                // Get where this card currently is visually
                const visualPosition = optimistic.positions.get(cardKey);
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
                optimistic.motions.delete(animEventKey('attack_pass', card, 'hand', 'table', seatOf(game)));
                optimistic.positions.delete(cardKey);
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
            optimistic.pass.current = { wire, cards, seat: seatOf(game) };
        }

        // 3. Await the server's verdict (revert-on-rejection below).
        try {
            return await serverPromise;
        } catch (error) {
            // Server rejected the pass - the cards go home and the pass is pending
            // nowhere, so no board is asked about it again.
            refused = true;
            optimistic.pass.current = null;
            // The cards land back in my hand, and the lead and the shield are as they were.
            const homeBoard = refusedBoard(game, (held) => {
                const back = withdrawn(held, cards);
                return back && turnedBoard(back, game.firstAttacker, game.defender);
            });

            // Check if conflict detection already handled these cards
            const cardsNeedingRevert = cards.filter(card =>
                optimistic.motions.has(animEventKey('attack_pass', card, 'hand', 'table', seatOf(game))));

            if (cardsNeedingRevert.length === 0) {
            } else {

                cardsNeedingRevert.forEach(card => {
                    const cardKey = getCardKey(card);
                    if (optimistic.reverting.has(cardKey)) return;
                    optimistic.reverting.add(cardKey);

                    const visualPosition = optimistic.positions.get(cardKey);
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

                    optimistic.motions.delete(animEventKey('attack_pass', card, 'hand', 'table', seatOf(game)));
                    optimistic.positions.delete(cardKey);
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
        const allTableCards = tableCards(game);

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
                optimistic.motions.has(animEventKey('pickup', card, 'table', 'hand', seatOf(game))));

            if (stillTracking.length === 0) {
                throw error;
            }
            // The table goes back to the board the pickup was made on, as long as no push has moved the game on.
            const homeBoard = refusedBoard(game, () => game);

            allTableCards.forEach(card => {
                const cardKey = getCardKey(card);
                if (optimistic.reverting.has(cardKey)) return;
                optimistic.reverting.add(cardKey);

                const visualPosition = optimistic.positions.get(cardKey);
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

                optimistic.motions.delete(animEventKey('pickup', card, 'table', 'hand', seatOf(game)));
                optimistic.positions.delete(cardKey);
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
                rememberPending(optimistic, 'cover', coverCard, 'hand', 'table', seatOf(game), timestamp);

                // Track visual position with target card info for animations
                const positionInfo: any = {
                    location: 'table',
                    seat: seatOf(game),
                    target_card: attackCard,
                    battle_index: battleIndex
                };
                optimistic.positions.set(cardKey, positionInfo);
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
                if (optimistic.reverting.has(cardKey)) return;
                optimistic.reverting.add(cardKey);

                const visualPosition = optimistic.positions.get(cardKey);
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

                optimistic.motions.delete(animEventKey('cover', card, 'hand', 'table', seatOf(game)));
                optimistic.positions.delete(cardKey);
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
            rowMs,
            heldPiles,
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
 