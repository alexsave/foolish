import React, { createContext, useContext, useEffect, useState, useRef, useCallback } from 'react';
import { Card, Game, GAME_STATUS, PLAYER_STATUS, PublicGame } from '@shared/types.ts';
import { useServer, useServerActions } from './ServerContext';
import { useAuth } from './AuthContext';
import { useParams } from 'next/navigation';
import supabase from '../backend/Connector';
import { ANIMATION_TIME } from '../constants/constants';
import { validateAttack, validatePass, validatePickup, validateCover, nextDefenderIndex } from '../utils/gameValidation';
import { getTableCards, cardsIntersection, getCardKeyPlayerId, createCardEventString, getCardKey } from '../utils/animationUtils';
import { animationFeed } from '../state/animationFeed';
import { staleOptimisticKeysOnTable } from '../state/optimisticAnimation';
import { resolveUnconfirmedAttackCovers } from '../state/optimisticConflicts';
import { optimisticOverlay } from '../state/optimisticOverlay';
import { shouldDropStaleSequence } from '../state/clientReconcile';

// Animation timing constant
export { ANIMATION_TIME } from '../constants/constants';

// Bot bump timeout - 20 seconds of no animations (currently unused)
// const BOT_BUMP_TIMEOUT = 20000;

//message type (currently unused)
// interface PayLoad {
//     type: 'animation_sequence';
//     sequence_id: string;
//     timestamp: number;
//     events: PublicAnimationEvent;
//     game: PublicGame;
// }

interface ClientAnimationEvent  {
    type: 'magic_transition' | 'deal' | 'flipped' | 'defender_move' | 'attack_pass' | 'cover' | 'pickup' | 'discard' | 'out' | 'refill' | 'cards_to_trash' | 'revert';
    player_id?: string;
    cards?: Card[];
    from_location?: 'deck' | 'hand' | 'table' | 'discard';
    to_location?: 'deck' | 'hand' | 'table' | 'discard' | 'flipped';
    target_card?: Card;
    target_cards?: Card[]; // For multi-card cover animations
    battle_index?: number;
    message?: string;
    game_state?: Game; // intermediate game state after this event
    is_revert?: boolean; // CLIENT-ONLY: flag for reverted optimistic animations
}

interface AnimationContextType {
    isAnimating: boolean;
    currentAnimation: ClientAnimationEvent | null;
    // Cards currently flying from the deck pile. Drives the visible pile size.
    // Drops BEFORE the animation starts and resets when the snapshot commits.
    inFlightFromDeck: number;
    // Subset of inFlightFromDeck that's headed to the flipped slot — these
    // are still "in the deck system" so they count toward the badge total.
    inFlightToFlipped: number;
    getCardAnimationState: (card: Card, playerId?: string) => {
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
// serialized each event's entire embedded `game_state` — the heaviest part of the
// payload — on every received message just to compare it. This signature captures
// the move-defining fields (type, player, from/to, cards, battle index) PLUS a few
// O(1) scalars off game_state (deck size, table size, own hand size) so two
// same-shaped-but-distinct sequences — e.g. two single-card refills at different
// deck sizes — still hash differently, the way the full stringify did, at a tiny
// fraction of the cost.
const eventsSignature = (events: any[]): string =>
    events
        .map((e) => {
            const gs = e.game_state;
            return [
                e.type,
                e.player_id ?? '',
                e.from_location ?? '',
                e.to_location ?? '',
                e.battle_index ?? '',
                (e.cards ?? []).map((c: Card) => `${c.suit}-${c.value}`).join(','),
                // cheap state discriminators (no deep serialization)
                gs?.deck_length ?? '',
                gs?.table_battles?.length ?? '',
                gs?.self?.hand?.length ?? '',
            ].join('|');
        })
        .join(';');

// Check if any bot can possibly move in the current game state
const canBotMove = (game: PublicGame | undefined): boolean => {
    if (!game || game.status !== GAME_STATUS.PLAYING) {
        return false;
    }

    const players = game.players;
    if (!players || players.length === 0) {
        return false;
    }

    const tableBattles = game.table_battles || [];
    const tableIsEmpty = tableBattles.length === 0;
    const hasUncoveredAttacks = tableBattles.some(b => !b.defense);
    const allCovered = tableBattles.length > 0 && !hasUncoveredAttacks;
    const goodPlayers = new Set(game.good_players || []);


    // Case 1: Table is empty - only first_attacker can move
    if (tableIsEmpty) {
        const firstAttacker = players[game.first_attacker];
        const result = firstAttacker?.is_ai === true;
        return result;
    }

    // Case 2: Table has cards
    const defender = players[game.defender];
    const defenderIsBot = defender?.is_ai === true;

    // Defender can move if there are uncovered attacks
    if (defenderIsBot && hasUncoveredAttacks) {
        return true;
    }

    // Attackers (non-defenders) can move if:
    // 1. There are uncovered attacks they can add to, OR
    // 2. All cards are covered but they haven't said "good" yet

    // Check if any bot attacker hasn't said good yet
    for (let i = 0; i < players.length; i++) {
        if (i === game.defender) continue; // Skip defender
        const player = players[i];
        if (player.is_ai && player.status === PLAYER_STATUS.IN) {
            // Bot attacker - check if they can still act
            if (!allCovered) {
                return true;
            }
            // All covered - can they say good?
            if (!goodPlayers.has(player.player_id)) {
                return true;
            }
        }
    }

    return false;
};

export const AnimationProvider = ({ children }: { children: React.ReactNode }) => {
    // Actions come from the stable actions context (identity never changes);
    // only the state this provider genuinely needs comes from the state context.
    const serverActions = useServerActions();
    const { updateGameState } = serverActions;
    const { games, game_id } = useServer();
    const { user_id } = useAuth();
    const url_game_id = useParams<{ game_id: string }>().game_id?.toLowerCase();

    const [isAnimating, setIsAnimating] = useState(false);
    const [currentAnimation, setCurrentAnimation] = useState<ClientAnimationEvent | null>(null);
    const [animationQueue, setAnimationQueue] = useState<ClientAnimationEvent[]>([]);
    const [inFlightFromDeck, setInFlightFromDeck] = useState(0);
    const [inFlightToFlipped, setInFlightToFlipped] = useState(0);

    // Keep refs in sync with state
    useEffect(() => {
        animationQueueRef.current = animationQueue;
    }, [animationQueue]);

    const [animatingCards, setAnimatingCards] = useState<Map<string, {
        animationType: string;
        progress: number;
        fromLocation: string | null;
        toLocation: string | null;
        startTime: number;
    }>>(new Map());

    const timeoutRef = useRef<NodeJS.Timeout | null>(null);
    const animationQueueRef = useRef<ClientAnimationEvent[]>([]);
    const pendingCompletionCallbackRef = useRef<(() => void) | null>(null);
    const remainingSequenceEventsRef = useRef<number>(0);

    // Bot bump timer ref
    const botBumpTimerRef = useRef<NodeJS.Timeout | null>(null);

    // Track if there have been bot moves in the last interval
    const hasBotMovedRef = useRef<boolean>(false);

    // Ref to track current game state (avoids stale closure in interval)
    const currentGameRef = useRef<typeof games[string] | undefined>(undefined);

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
    // Map of cardKey -> { location: 'table' | 'hand', playerId: string, target_card?: Card, battle_index?: number }
    const optimisticCardPositions = useRef<Map<string, { location: string, playerId: string, target_card?: Card, battle_index?: number }>>(new Map());

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
    // <= this one (strictly superseded — each sequence carries the full resulting
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
        }
    }, [url_game_id, games]);

    // Keep currentGameRef in sync with latest game state
    useEffect(() => {
        currentGameRef.current = url_game_id ? games[url_game_id] : undefined;
    }, [url_game_id, games]);

    // Start bot bump timer when component mounts and game is loaded
    useEffect(() => {
        if (!url_game_id) {
            return;
        }

        // Check if there are any AI players in the game
        const currentGame = games[url_game_id];
        const hasAIPlayers = currentGame?.players?.some(player => player.is_ai) || false;

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
    }, [url_game_id, url_game_id ? games[url_game_id]?.players?.length : 0]);

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

        const serverState = lastEventWithState.game_state;
        const myPlayerId = serverState.self?.player_id || user_id;

        // Check if server's final state already includes my optimistic cards
        // If so, they were accepted! Don't revert.
        const serverTableCards = getTableCards(serverState);


        // Find MY optimistic cards (attacks, covers, pickups)
        const myOptimisticAttackCovers: Card[] = [];
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
                    player_id: myPlayerId,
                    is_revert: true,
                    game_state: null as any
                });

                const cardEventString = createCardEventString('attack_pass', optCard, 'hand', 'table', myPlayerId);
                optimisticAnimations.current.delete(cardEventString);
            });
        };

        optimisticAnimations.current.forEach((timestamp, cardEventString) => {
            try {
                const parsedEvent = JSON.parse(cardEventString);
                if (parsedEvent.player_id === myPlayerId) {
                    // Attacks and covers (hand → table)
                    if ((parsedEvent.type === 'attack_pass' || parsedEvent.type === 'cover') &&
                        parsedEvent.from_location === 'hand' &&
                        parsedEvent.to_location === 'table') {
                        myOptimisticAttackCovers.push(parsedEvent.card);
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

        // Count uncovered attacks on server's table (before pass)
        const serverTableBattles = serverState?.table_battles || [];
        const serverUncoveredAttacks = serverTableBattles.filter((b: any) => !b.defense).length;

        // ====== CHECK FOR OPTIMISTIC PASS CONFLICTS EARLY ======
        // Do this BEFORE merging, so invalid pass cards don't get baked into states
        if (optimisticPassState.current && message.events.length > 0 && serverAttackPasses.length > 0) {
            const nextDefenderId = optimisticPassState.current.defender;
            const finalGameState = message.game || serverState;
            const nextDefenderHandSize = finalGameState?.players?.[nextDefenderId]?.hand_length ?? 0;

            const serverAttackCards = serverAttackPasses.reduce((sum: number, evt: any) => sum + (evt.cards?.length || 0), 0);

            const optimisticPassCards = Array.from(optimisticAnimations.current.keys())
                .filter(key => {
                    try {
                        const parsed = JSON.parse(key);
                        return parsed.type === 'attack_pass' &&
                            parsed.player_id === myPlayerId &&
                            optimisticCardPositions.current.has(getCardKey(parsed.card));
                    } catch {
                        return false;
                    }
                })
                .length;

            const totalAttacksIfPassSucceeds = serverUncoveredAttacks + serverAttackCards + optimisticPassCards;

            if (totalAttacksIfPassSucceeds > nextDefenderHandSize) {
                passIsInvalid = true;

                // Collect pass cards to revert
                const passCardsToRevert: Card[] = [];
                optimisticAnimations.current.forEach((timestamp, key) => {
                    try {
                        const parsed = JSON.parse(key);
                        if (parsed.type === 'attack_pass' && parsed.player_id === myPlayerId) {
                            const cardId = getCardKey(parsed.card);
                            if (optimisticCardPositions.current.has(cardId)) {
                                passCardsToRevert.push(parsed.card);
                            }
                        }
                    } catch (e) { }
                });

                if (passCardsToRevert.length > 0) {
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
                        player_id: myPlayerId,
                        is_revert: true,
                        game_state: null as any // Will be set later
                    });

                    // Clear optimistic pass state and card tracking
                    optimisticPassState.current = null;

                    passCardsToRevert.forEach(card => {
                        const cardEventString = createCardEventString('attack_pass', card, 'hand', 'table', myPlayerId);
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
            // Get the pass event (the attack_pass event that caused defender to change)

            const serverPassEvent = serverAttackPasses[0];

            const finalGameState = message.game || serverState;
            const newDefenderId = serverDefenderAfter; // After pass

            // Check 1: Did the pass make the attacker become the defender?
            if (newDefenderId !== undefined) {
                // Find my player index
                const myPlayerIndex = finalGameState?.players?.findIndex((p: any) =>
                    p.player_id === myPlayerId
                );

                if (myPlayerIndex === newDefenderId) {
                    // Revert all optimistic attacks
                    revertOptimisticAttackCovers();

                    // Remove from merge list
                    myOptimisticAttackCovers.length = 0;
                }
            }

            // Check 2: Does the new defender have exactly enough cards for the table?
            if (myOptimisticAttackCovers.length > 0 && newDefenderId !== undefined) {
                const newDefenderHandSize = finalGameState?.players?.[newDefenderId]?.hand_length ?? 0;
                const serverPassCards = serverPassEvent.cards?.length || 0;

                // Total attacks the new defender will face = uncovered + pass cards
                const totalAttacksAfterPass = serverUncoveredAttacks + serverPassCards;


                // If table is full (new defender has exactly enough cards), no more attacks allowed
                if (totalAttacksAfterPass >= newDefenderHandSize) {
                    // Revert all optimistic attacks
                    revertOptimisticAttackCovers();

                    // Remove from merge list
                    myOptimisticAttackCovers.length = 0;
                }
            }
        }

        // Handle optimistic pickup conflicts
        if (myOptimisticPickups.length > 0) {
            // Two scenarios:
            // 1. Server shows cards still on table (attack came in) - revert to table
            // 2. Server sends cards_to_trash/discard (good was played) - revert to table before trash

            const hasCardsToTrash = message.events.some((evt: any) =>
                evt.type === 'cards_to_trash' || evt.type === 'discard'
            );

            let pickupCardsToRevert: Card[] = [];

            if (hasCardsToTrash) {
                // Good was played - ALL optimistic pickups need to be reverted
                pickupCardsToRevert = myOptimisticPickups;
            } else if (serverTableCards.length > 0) {
                // Attack came in - only revert cards that are still on table
                pickupCardsToRevert = cardsIntersection(myOptimisticPickups, serverTableCards);
            }

            if (pickupCardsToRevert.length > 0) {
                // Mark all cards as reverting and clear tracking
                pickupCardsToRevert.forEach(card => {
                    const cardKey = getCardKey(card);
                    revertingCards.current.add(cardKey);

                    // Clear tracking
                    const cardEventString = createCardEventString('pickup', card, 'table', 'hand', myPlayerId);
                    optimisticAnimations.current.delete(cardEventString);
                });

                // Create SINGLE revert event with ALL cards
                revertEvents.push({
                    type: 'revert',
                    cards: pickupCardsToRevert, // ALL cards in one event
                    from_location: 'hand',
                    to_location: 'table',
                    player_id: myPlayerId,
                    is_revert: true,
                    game_state: null as any // Will be set later
                });
            }
        }

        if (myOptimisticAttackCovers.length > 0 && myOptimisticCardsAccepted.length === 0) {
            // Server didn't include our optimistic cards yet. Decide per card whether
            // each was genuinely never accepted (revert to hand) or is simply not yet
            // confirmed on THIS (possibly concurrent / pre-our-commit) broadcast and
            // should be kept (merged) — see optimisticConflicts.ts. This is the same
            // decision the deployed client and the e2e suite both exercise.
            const { revert: cardsToRevert, merge: cardsToMerge, clear: cardsToClear } = resolveUnconfirmedAttackCovers(
                myOptimisticAttackCovers,
                serverTableCards,
                message.events,
                message.game || serverState,
            );

            // Cards that were accepted then swept off the table by this broadcast's
            // own pickup/trash: drop their optimistic tracking with NO revert — the
            // clear event animates them off the table (was the "someone picked up my
            // card and it flew back to my hand" flicker).
            cardsToClear.forEach((card: Card) => {
                const cardKey = getCardKey(card);
                optimisticAnimations.current.delete(createCardEventString('attack_pass', card, 'hand', 'table', myPlayerId));
                optimisticAnimations.current.delete(createCardEventString('cover', card, 'hand', 'table', myPlayerId));
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
                        player_id: myPlayerId,
                        is_revert: true,
                        message: 'Attack invalidated by earlier attack'
                    });

                    // Clear from optimistic tracking
                    const cardEventString = createCardEventString('attack_pass', card, 'hand', 'table', myPlayerId);
                    optimisticAnimations.current.delete(cardEventString);
                });
            }

            if (cardsToMerge.length > 0) {
                    // Get my player ID
                    const myPlayerId = serverState.self?.player_id || user_id;

                    // Merge optimistic cards into ALL game states (events + final)
                    const statesToMerge = [
                        ...message.events.map((evt: any) => evt.game_state).filter(Boolean),
                        message.game
                    ].filter(Boolean);

                    // Check if any pass events in this message are from the current user
                    const hasUserPass = message.events.some((evt: any) =>
                        evt.type === 'attack_pass' && evt.player_id === user_id
                    );

                    statesToMerge.forEach((state: any, stateIdx: number) => {
                        if (state && state.table_battles) {
                            // FIRST: Preserve optimistic pass state if present AND this message contains our pass
                            // Otherwise, trust the server's game state (which is correct for other players' passes)
                            if (optimisticPassState.current && hasUserPass) {
                                state.defender = optimisticPassState.current.defender;
                                state.first_attacker = optimisticPassState.current.first_attacker;
                            }

                            cardsToMerge.forEach((optCard: Card) => {
                                const alreadyPresent = state.table_battles.some((b: any) =>
                                    (b.attack.suit === optCard.suit && b.attack.value === optCard.value) ||
                                    (b.defense && b.defense.suit === optCard.suit && b.defense.value === optCard.value)
                                );
                                if (!alreadyPresent) {
                                    const cardKey = getCardKey(optCard);
                                    const positionInfo = optimisticCardPositions.current.get(cardKey);

                                    // Check if this is a cover (has target_card info)
                                    if (positionInfo?.target_card) {
                                        // This is a cover - add as defense to the correct battle
                                        const targetCard = positionInfo.target_card;
                                        const battleIdx = state.table_battles.findIndex((b: any) =>
                                            b.attack.suit === targetCard.suit && b.attack.value === targetCard.value
                                        );
                                        if (battleIdx >= 0 && !state.table_battles[battleIdx].defense) {
                                            state.table_battles[battleIdx].defense = optCard;
                                        }
                                    } else {
                                        // This is an attack - add as new battle
                                        state.table_battles.push({
                                            attack: optCard,
                                            defense: null
                                        });
                                    }
                                }

                                // CRITICAL: Also remove this card from my hand in this state!
                                // Find my player in this state
                                if (state.self?.player_id === myPlayerId && state.self.hand) {
                                    state.self.hand = state.self.hand.filter((c: Card) =>
                                        !(c.suit === optCard.suit && c.value === optCard.value)
                                    );
                                }

                                // Also check players array
                                if (state.players) {
                                    state.players.forEach((p: any, idx: number) => {
                                        if (p.player_id === myPlayerId && p.hand) {
                                            p.hand = p.hand.filter((c: Card) =>
                                                !(c.suit === optCard.suit && c.value === optCard.value)
                                            );
                                            // Update hand_length too
                                            p.hand_length = p.hand.length;
                                        }
                                    });
                                }
                            });
                        }
                    });
                }
        }

        return { revertEvents, passIsInvalid };
    }

    // Handle animation messages from real-time channel
    const handleAnimationMessage = (message: any) => {
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

            // Release any of my optimistic cards that this AUTHORITATIVE state
            // confirms are on the table BUT whose confirming broadcast was
            // dropped by the version gate (i.e. NOT named by this broadcast's
            // own events). Cards this broadcast DOES name are deliberately left
            // for the per-event dedup below — releasing them here first would
            // make their own confirming event look un-optimistic and animate a
            // second time (the double-play bug). message.game is the pristine
            // server state here (resolveOptimisticConflicts hasn't injected yet).
            if (message.game?.table_battles && optimisticAnimations.current.size > 0) {
                const tableCards: Card[] = [];
                for (const b of message.game.table_battles) {
                    if (b?.attack) tableCards.push(b.attack);
                    if (b?.defense) tableCards.push(b.defense);
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
        currentGameIdRef.current = message.game.id;

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
                const cardEventString = createCardEventString(serverEvent.type, card, serverEvent.from_location, serverEvent.to_location, serverEvent.player_id);
                const timestamp = optimisticAnimations.current.get(cardEventString);
                const isOptimistic = timestamp !== undefined;
                return isOptimistic;
            });

            if (allCardsOptimistic) {
                optimisticEventIndices.push(eventIndex);
                // Clear the optimistic animations since server confirmed them
                serverEvent.cards.forEach((card: Card) => {
                    const cardKey = getCardKey(card);
                    const cardEventString = createCardEventString(serverEvent.type, card, serverEvent.from_location, serverEvent.to_location, serverEvent.player_id);
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
                updateGameState(message.game.id, message.game);
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
            
            // Check if any pass events in this message are from the current user
            const hasUserPass = message.events.some((evt: any) => 
                evt.type === 'attack_pass' && evt.player_id === user_id
            );
            
            // Only apply optimistic pass state if this message contains a pass from the current user
            // Otherwise, trust the server's game state (which is correct for other players' passes)
            if (optimisticPassState.current && hasUserPass) {
                // Check if server confirmed optimistic pass
                const serverConfirmedPass =
                    message.game.defender === optimisticPassState.current.defender &&
                    message.game.first_attacker === optimisticPassState.current.first_attacker;

                if (serverConfirmedPass) {
                    optimisticPassState.current = null;
                } else {
                    // Server didn't confirm - use optimistic state (our pass might have been rejected or modified)
                    message.game.defender = optimisticPassState.current.defender;
                    message.game.first_attacker = optimisticPassState.current.first_attacker;
                }
            } else if (optimisticPassState.current && !hasUserPass) {
                // This message doesn't contain our pass, so clear stale optimistic state
                // and trust the server (another player passed)
                optimisticPassState.current = null;
            }

            updateGameState(message.game.id, message.game);
        };
        remainingSequenceEventsRef.current = message.events.length + revertEvents.length;

        if (revertEvents.length === 0) {
            // Queue all events from the sequence
            setAnimationQueue(prev => [...prev, ...message.events]);
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

        let baseState;
        if (hasPickupRevertsForState) {
            // For pickup reverts, we need state with cards on table
            if (magicTransitionEvent?.game_state) {
                // Use magic_transition state (has cards on table before good)
                baseState = magicTransitionEvent.game_state;
            } else if (pickupEvent?.cards) {
                // Reconstruct state with cards on table (before pickup)
                baseState = serverStateForRevert ? JSON.parse(JSON.stringify(serverStateForRevert)) : null;
                if (baseState && pickupEvent.cards) {
                    // Put the cards back on the table as uncovered attacks
                    baseState.table_battles = pickupEvent.cards.map((card: Card) => ({
                        attack: card,
                        defense: null
                    }));
                }
            } else {
                baseState = serverStateForRevert;
            }
        } else {
            baseState = serverStateForRevert;
        }

        const stateWithOptimistic = baseState ? JSON.parse(JSON.stringify(baseState)) : null;

        // Check if we have pass reverts - they need original defender value
        const hasPassReverts = revertEvents.some(rev =>
            rev.to_location === 'hand' &&
            passIsInvalid // We detected an invalid pass earlier
        );

        if (hasPassReverts && stateWithOptimistic && serverStateForRevert) {
            // For pass reverts, use the SERVER's defender value (original before pass)
            stateWithOptimistic.defender = serverStateForRevert.defender;
            stateWithOptimistic.first_attacker = serverStateForRevert.first_attacker;
        }

        if (stateWithOptimistic) {
            // IMPORTANT: Remove BOTH optimistic cards AND cards that will be animated
            // This prevents the "transform" issue where invalid card becomes valid card

            const revertCardKeys = new Set(
                revertEvents.flatMap(evt => evt.cards?.map(c => getCardKey(c)) || [])
            );

            // For attack conflicts: remove cards that will be animated (valid attacks)
            const serverAttackCards = new Set(
                message.events
                    .filter((evt: any) => evt.type === 'attack_pass' && evt.from_location === 'hand')
                    .flatMap((evt: any) => evt.cards?.map((c: Card) => getCardKey(c)) || [])
            );

            // Check if we have pickup reverts (hand → table) vs attack reverts (table → hand)
            const hasPickupRevertsForClean = revertEvents.some(rev => rev.to_location === 'table');
            const hasAttackReverts = revertEvents.some(rev => rev.to_location === 'hand');
            const hasPickupEventForClean = message.events.some((evt: any) => evt.type === 'pickup' || evt.type === 'cards_to_trash');

            if (hasPickupRevertsForClean) {
                // PICKUP REVERT SCENARIO (hand → table): 
                // State should show reverted cards on table, but NOT server attack cards that will animate
                stateWithOptimistic.table_battles = (stateWithOptimistic.table_battles || []).filter((b: any) => {
                    const attackKey = getCardKey(b.attack);
                    const defenseKey = b.defense ? getCardKey(b.defense) : null;

                    // Remove server attack cards that will animate (they shouldn't appear yet)
                    const removeAttack = serverAttackCards.has(attackKey);
                    const removeDefense = defenseKey && serverAttackCards.has(defenseKey);

                    return !removeAttack && !removeDefense;
                });
            } else if (hasPickupEventForClean && hasAttackReverts) {
                // ATTACK REVERT + PICKUP SCENARIO: Only remove reverting cards, keep everything else
                // (Cards to be picked up need to stay on table for pickup animation)
                stateWithOptimistic.table_battles = (stateWithOptimistic.table_battles || []).filter((b: any) => {
                    const attackKey = getCardKey(b.attack);
                    const defenseKey = b.defense ? getCardKey(b.defense) : null;

                    // Only remove reverting cards
                    const removeAttack = revertCardKeys.has(attackKey);
                    const removeDefense = defenseKey && revertCardKeys.has(defenseKey);

                    return !removeAttack && !removeDefense;
                });
            } else {
                // ATTACK CONFLICT SCENARIO: Remove both reverting AND valid attacks that will animate
                stateWithOptimistic.table_battles = (stateWithOptimistic.table_battles || []).filter((b: any) => {
                    const attackKey = getCardKey(b.attack);
                    const defenseKey = b.defense ? getCardKey(b.defense) : null;

                    // Remove reverting cards OR cards that will animate
                    const removeAttack = revertCardKeys.has(attackKey) || serverAttackCards.has(attackKey);
                    const removeDefense = defenseKey && (revertCardKeys.has(defenseKey) || serverAttackCards.has(defenseKey));

                    return !removeAttack && !removeDefense;
                });
            }

            // For pass reverts (table → hand), add cards back to player's hand
            if (hasPassReverts && stateWithOptimistic.self) {
                const passRevertCards = revertEvents
                    .filter(rev => rev.to_location === 'hand')
                    .flatMap(rev => rev.cards || []);

                if (passRevertCards.length > 0) {
                    // Add cards back to hand
                    stateWithOptimistic.self.hand = stateWithOptimistic.self.hand || [];
                    passRevertCards.forEach((card: Card) => {
                        // Only add if not already in hand
                        const alreadyInHand = stateWithOptimistic.self.hand.some((c: Card) =>
                            c.suit === card.suit && c.value === card.value
                        );
                        if (!alreadyInHand) {
                            stateWithOptimistic.self.hand.push(card);
                        }
                    });
                }
            }
        }

        revertEvents.forEach((revertEvent, idx) => {
            revertEvent.game_state = stateWithOptimistic as any;
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

            setAnimationQueue(prev => [...prev, ...queueOrder]);
        } else if (firstAttackIndex >= 0) {
            // Queue reverts IMMEDIATELY before the valid attack for parallel visual effect
            const eventsBeforeAttack = message.events.slice(0, firstAttackIndex);
            const restEvents = message.events.slice(firstAttackIndex);

            const queueOrder = [
                ...eventsBeforeAttack,
                ...revertEvents,    // Revert animates
                ...restEvents       // Valid attack animates right after (looks parallel)
            ];

            setAnimationQueue(prev => [...prev, ...queueOrder]);
        } else if (firstPickupIndex >= 0) {
            // Queue reverts before the pickup so card goes back to hand first
            const eventsBeforePickup = message.events.slice(0, firstPickupIndex);
            const restEvents = message.events.slice(firstPickupIndex);

            const queueOrder = [
                ...eventsBeforePickup,
                ...revertEvents,    // Revert animates first
                ...restEvents       // Then pickup animates
            ];

            setAnimationQueue(prev => [...prev, ...queueOrder]);
        } else {
            setAnimationQueue(prev => [...prev, ...revertEvents, ...message.events]);
        }
    };


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

        // Check if this animation is from a bot player
        if (nextAnimation.player_id && url_game_id) {
            const currentGame = games[url_game_id];
            const player = currentGame?.players?.find(p => p.player_id === nextAnimation.player_id);
            if (player?.is_ai) {
                // This is a bot move - set the flag
                hasBotMovedRef.current = true;
            }
        }

        setCurrentAnimation(nextAnimation);
        setAnimationQueue(prev => prev.slice(1));
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

        // Start tracking cards in this animation (simplified - CSS handles the actual animation)
        if (nextAnimation.cards && nextAnimation.cards.length > 0) {
            setAnimatingCards(prev => {
                const newAnimatingCards = new Map(prev);

                nextAnimation.cards!.forEach(card => {
                    const cardKey = getCardKeyPlayerId(card, nextAnimation.player_id);
                    newAnimatingCards.set(cardKey, {
                        animationType: nextAnimation.type,
                        progress: 1, // Always 1 - CSS transitions handle the animation
                        fromLocation: nextAnimation.from_location || null,
                        toLocation: nextAnimation.to_location || null,
                        startTime: Date.now()
                    });
                });

                return newAnimatingCards;
            });
        }

        // Animation duration: use ANIMATION_TIME constant for consistency
        timeoutRef.current = setTimeout(() => {
            // UPDATE THE GAME STATE WITH THE INTERMEDIATE STATE AFTER ANIMATION COMPLETES
            if (nextAnimation.game_state && currentGameIdRef.current) {

                // If we have an optimistic pass, preserve defender/first_attacker
                if (optimisticPassState.current) {
                    nextAnimation.game_state.defender = optimisticPassState.current.defender;
                    nextAnimation.game_state.first_attacker = optimisticPassState.current.first_attacker;
                }

                updateGameState(currentGameIdRef.current, nextAnimation.game_state);
            }

            // Cards have landed; game.deck_length now reflects the reduction, so
            // clear the in-flight counts to avoid double-counting during the gap.
            setInFlightFromDeck(0);
            setInFlightToFlipped(0);

            // Remove cards from animating state
            if (nextAnimation.cards) {
                setAnimatingCards(prev => {
                    const updated = new Map(prev);
                    nextAnimation.cards!.forEach(card => {
                        const cardKey = getCardKeyPlayerId(card, nextAnimation.player_id);
                        updated.delete(cardKey);
                    });
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

            // Inter-event gap. This is NOT pure dead air — it's coupled to the
            // AnimationOverlay's per-event lifecycle, which ends by scheduling a "clear
            // overlay" timeout. If the previous event's clear fires AFTER the next event
            // has created its cards, it wipes them mid-flight (cards teleport; multi-card
            // deals lose every card after the first). With the overlay clearing at
            // ANIMATION_TIME (see AnimationOverlay), the safety margin before the next
            // event's cards are created is (gap + ANIMATION_TIME) − (overlay clear) and
            // works out to ~gap ms, so this 25ms keeps a real (if small) margin. The two
            // constants are a matched pair — don't lower one without the other.
            setTimeout(processAnimationQueue, 25);
        }, ANIMATION_TIME);
    }, [updateGameState, url_game_id, games]);

    // Start processing queue when items are added and no animation is running
    useEffect(() => {
        if (animationQueue.length > 0 && !isAnimating) {
            processAnimationQueue();
        }
    }, [animationQueue, isAnimating, processAnimationQueue]);

    // Queue a single animation
    const queueAnimation = (event: ClientAnimationEvent) => {
        setAnimationQueue(prev => {
            const newQueue = [...prev, event];
            return newQueue;
        });
    };

    // Get animation state for a specific card
    const getCardAnimationState = (card: Card, playerId?: string) => {
        const cardKey = getCardKeyPlayerId(card, playerId);
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
    const triggerOptimisticAnimation = (animationType: string, cards: Card[], fromLocation: string, toLocation: string, playerId?: string, targetCard?: Card, battleIndex?: number) => {
        const animationEvent: ClientAnimationEvent = {
            type: animationType as any,
            cards: cards,
            from_location: fromLocation as any,
            to_location: toLocation as any,
            player_id: playerId,
            target_card: targetCard,
            battle_index: battleIndex,
            message: `Optimistic ${animationType} animation`
        };

        // Track EACH CARD individually to avoid duplicates from server
        // Server may split multi-card actions into separate events (one per card)
        const timestamp = Date.now();
        cards.forEach(card => {
            const cardKey = getCardKey(card);
            const cardEventString = createCardEventString(animationType, card, fromLocation, toLocation, playerId);
            optimisticAnimations.current.set(cardEventString, timestamp);

            // Track visual position for revert animations
            // After animation completes, card will VISUALLY be at toLocation
            const positionInfo: any = {
                location: toLocation,
                playerId: playerId || ''
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
    // 3. ServerContext does optimistic game state updates after ANIMATION_TIME (UI consistency)  
    // 4. Server response with intermediate states provides final truth

    // Game action methods that handle optimistic animations + server calls
    const attack = async (cards: Card[]): Promise<{ game_id: string }> => {
        if (!game_id || !games[game_id]) {
            throw new Error('No active game');
        }

        const game = games[game_id];

        // 1. Send the request BEFORE validating — the server is authoritative and
        //    rejects illegal moves, so we don't block the round-trip on local
        //    validation. `valid` is captured by the server method's deferred
        //    optimistic patch (applied only if still valid) and gates the optimistic
        //    animation below.
        let valid = true;
        const serverPromise = serverActions.attack(cards, () => valid);

        // 2. Validate locally; only add optimistic feedback if the move is legal.
        try {
            validateAttack(game, cards);
        } catch {
            valid = false;
        }
        if (valid) {
            triggerOptimisticAnimation('attack_pass', cards, 'hand', 'table', game.self?.player_id);
        }

        // 3. Await the server's verdict (revert-on-rejection below).
        try {
            return await serverPromise;
        } catch (error) {
            // Server rejected the attack - but check if we already reverted due to conflict detection

            cards.forEach(card => {
                const cardKey = getCardKey(card);

                // Check if this card is already being reverted OR tracking was cleared
                const isCurrentlyReverting = revertingCards.current.has(cardKey);
                const wasAlreadyReverted = !optimisticCardPositions.current.has(cardKey);

                // Also check if optimistic animation was cleared (conflict detection clears it)
                const attackCardEventString = createCardEventString('attack_pass', card, 'hand', 'table', game.self?.player_id);
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
                    player_id: game.self?.player_id,
                    is_revert: true,
                    message: 'Attack rejected by server'
                };

                queueAnimation(revertEvent);

                // Clear from optimistic tracking
                const fallbackCardEventString = createCardEventString('attack_pass', card, 'hand', 'table', game.self?.player_id);
                optimisticAnimations.current.delete(fallbackCardEventString);
            });

            throw error;
        }
    };

    const pass = async (cards: Card[]): Promise<{ game_id: string }> => {
        if (!game_id || !games[game_id]) {
            throw new Error('No active game');
        }

        const game = games[game_id];

        // 1. Send the request BEFORE validating (server is authoritative; see attack).
        let valid = true;
        const serverPromise = serverActions.pass(cards, () => valid);

        // 2. Validate locally; only add optimistic feedback if the move is legal.
        try {
            validatePass(game, cards);
        } catch {
            valid = false;
        }
        if (valid) {
            // Trigger optimistic animation - single animation with all cards going to their spots
            triggerOptimisticAnimation('attack_pass', cards, 'hand', 'table', game.self?.player_id);

            // Track optimistic pass state (defender will change to next player).
            // Pass moves defender to the next IN-PLAY player (skipping eliminated
            // seats, exactly like the server's get_next_player_index); first_attacker
            // does NOT change during a pass (only changes on new round).
            optimisticPassState.current = {
                defender: nextDefenderIndex(game),
                first_attacker: game.first_attacker  // Unchanged
            };
        }

        // 3. Await the server's verdict (revert-on-rejection below).
        try {
            return await serverPromise;
        } catch (error) {
            // Server rejected the pass - clear optimistic pass state and create revert animation (if not already handled)
            optimisticPassState.current = null;

            // Check if conflict detection already handled these cards
            const cardsNeedingRevert = cards.filter(card => {
                const cardEventString = createCardEventString('attack_pass', card, 'hand', 'table', game.self?.player_id);
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
                        player_id: game.self?.player_id,
                        is_revert: true,
                        message: 'Pass rejected by server'
                    });

                    optimisticAnimations.current.delete(createCardEventString('attack_pass', card, 'hand', 'table', game.self?.player_id));
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

        // 1. Send the request BEFORE validating (server is authoritative; see attack).
        let valid = true;
        const serverPromise = serverActions.pickup(() => valid);

        // 2. Validate locally; only add optimistic feedback if the move is legal.
        try {
            validatePickup(game);
        } catch {
            valid = false;
        }
        if (valid) {
            triggerOptimisticAnimation('pickup', allTableCards, 'table', 'hand', game.self?.player_id);
        }

        // 3. Await the server's verdict (revert-on-rejection below).
        try {
            return await serverPromise;
        } catch (error) {
            // Server rejected the pickup
            // Check if conflict detection already handled reverts
            const stillTracking = allTableCards.filter(card => {
                const cardEventString = createCardEventString('pickup', card, 'table', 'hand', game.self?.player_id);
                return optimisticAnimations.current.has(cardEventString);
            });

            if (stillTracking.length === 0) {
                throw error;
            }

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
                    player_id: game.self?.player_id,
                    is_revert: true,
                    message: 'Pickup rejected by server'
                });

                optimisticAnimations.current.delete(createCardEventString('pickup', card, 'table', 'hand', game.self?.player_id));
            });
            throw error;
        }
    };

    const cover = async (coverCards: Card[], attackCards: Card[]): Promise<{ game_id: string }> => {
        if (!game_id || !games[game_id]) {
            throw new Error('No active game');
        }

        const game = games[game_id];

        // 1. Send the request BEFORE validating (server is authoritative; see attack).
        let valid = true;
        const serverPromise = serverActions.cover(coverCards, attackCards, () => valid);

        // 2. Validate locally; only add optimistic feedback if the move is legal.
        try {
            validateCover(game, coverCards, attackCards);
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
                player_id: game.self?.player_id,
                message: 'Optimistic cover animation'
            };

            // Track EACH CARD individually with its target attack card
            const timestamp = Date.now();
            coverCards.forEach((coverCard, idx) => {
                const attackCard = attackCards[idx];
                const cardKey = getCardKey(coverCard);

                // Find battle index for this attack
                const battleIndex = game.table_battles.findIndex(b =>
                    b.attack.suit === attackCard.suit && b.attack.value === attackCard.value
                );

                // Track for conflict detection
                const cardEventString = createCardEventString('cover', coverCard, 'hand', 'table', game.self?.player_id);
                optimisticAnimations.current.set(cardEventString, timestamp);

                // Track visual position with target card info for animations
                const positionInfo: any = {
                    location: 'table',
                    playerId: game.self?.player_id || '',
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
                    player_id: game.self?.player_id,
                    is_revert: true,
                    message: 'Cover rejected by server'
                });

                optimisticAnimations.current.delete(createCardEventString('cover', card, 'hand', 'table', game.self?.player_id));
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
 