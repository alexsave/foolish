import React, { createContext, useContext, useEffect, useState, useRef, useCallback } from 'react';
import { Card, Game } from '../common/types';
import { useServer } from './ServerContext';
import { useAuth } from './AuthContext';
import { useParams } from 'react-router-dom';
import supabase from '../backend/Connector';
import { ANIMATION_TIME } from '../constants/constants';
import { validateAttack, validatePass, validatePickup, validateCover } from '../utils/gameValidation';

// Animation timing constant
export { ANIMATION_TIME } from '../constants/constants';

// Bot bump timeout - 20 seconds of no animations
const BOT_BUMP_TIMEOUT = 20000;

interface AnimationEvent {
    type: 'magic_transition' | 'deal' | 'flipped' | 'defender_move' | 'attack_pass' | 'cover' | 'pickup' | 'discard' | 'out' | 'refill' | 'cards_to_trash';
    player_id?: string;
    cards?: Card[];
    from_location?: 'deck' | 'hand' | 'table' | 'discard';
    to_location?: 'deck' | 'hand' | 'table' | 'discard' | 'flipped';
    target_card?: Card;
    target_cards?: Card[]; // For multi-card cover animations
    battle_index?: number;
    message?: string;
    game_state?: Game; // intermediate game state after this event
}

// Client-side animation event with additional fields for UI logic
interface ClientAnimationEvent extends Omit<AnimationEvent, 'type'> {
    type: 'magic_transition' | 'deal' | 'flipped' | 'defender_move' | 'attack_pass' | 'cover' | 'pickup' | 'discard' | 'out' | 'refill' | 'cards_to_trash' | 'revert';
    is_revert?: boolean; // CLIENT-ONLY: flag for reverted optimistic animations
}

interface AnimationSequence {
    type: 'animation_sequence';
    events: AnimationEvent[];
    sequence_id: string;
    timestamp: number;
}

interface AnimationContextType {
    isAnimating: boolean;
    currentAnimation: ClientAnimationEvent | null;
    animationQueue: ClientAnimationEvent[];
    queueAnimation: (event: ClientAnimationEvent) => void;
    queueAnimationSequence: (sequence: AnimationSequence) => void;
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
}

const AnimationContext = createContext<AnimationContextType | null>(null);

export const AnimationProvider = ({ children }: { children: React.ReactNode }) => {
    const { updateGameState, games, game_id, ...serverMethods } = useServer();
    const { user_id } = useAuth();
    const url_game_id = useParams().game_id?.toLowerCase();
    
    const [isAnimating, setIsAnimating] = useState(false);
    const [currentAnimation, setCurrentAnimation] = useState<ClientAnimationEvent | null>(null);
    const [animationQueue, setAnimationQueue] = useState<ClientAnimationEvent[]>([]);
    
    // Keep refs in sync with state
    useEffect(() => {
        animationQueueRef.current = animationQueue;
    }, [animationQueue]);
    
    useEffect(() => {
        isAnimatingRef.current = isAnimating;
    }, [isAnimating]);
    const [animatingCards, setAnimatingCards] = useState<Map<string, {
        animationType: string;
        progress: number;
        fromLocation: string | null;
        toLocation: string | null;
        startTime: number;
    }>>(new Map());
    
    const timeoutRef = useRef<NodeJS.Timeout | null>(null);
    const animationQueueRef = useRef<ClientAnimationEvent[]>([]);
    const isAnimatingRef = useRef<boolean>(false);
    const pendingCompletionCallbackRef = useRef<(() => void) | null>(null);
    const remainingSequenceEventsRef = useRef<number>(0);

    // Bot bump timer ref
    const botBumpTimerRef = useRef<NodeJS.Timeout | null>(null);
    
    // Track if there have been bot moves in the last interval
    const hasBotMovedRef = useRef<boolean>(false);

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
    
    // Store channel reference for proper cleanup
    const gameUserChannelRef = useRef<any>(null);
    
    // Simple retry interval for animation channel
    const animationChannelRetryInterval = useRef(500); // Start with 0.5 seconds
    const MAX_RETRY_INTERVAL = 5000; // Cap at 5 seconds

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
        
        // Start interval that checks every 15 seconds
        const intervalId = setInterval(() => {
            // Check if there have been bot moves in the last 15 seconds
            if (!hasBotMovedRef.current) {
                // No bot moves - call bot_bump to wake up the bot loop
                console.log('No bot activity detected, calling bot_bump');
                supabase.functions.invoke('bot_bump', { 
                    body: { game_id: url_game_id } 
                }).catch(error => {
                    console.error('Bot bump failed:', error);
                });
            } else {
                console.log('Bot activity detected, skipping bot_bump');
            }
            
            // Reset the flag for the next interval
            hasBotMovedRef.current = false;
        }, 15000); // 15 seconds
        
        // Store the interval ID for cleanup
        botBumpTimerRef.current = intervalId as any;
        
        // Cleanup on unmount
        return () => {
            if (botBumpTimerRef.current) {
                clearInterval(botBumpTimerRef.current);
            }
        };
    }, [url_game_id, games]);

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

    // Subscribe to game-user channel for animation events
    useEffect(() => {
        if (!user_id || !url_game_id) {
            return;
        }

        const subscribeToGameAnimations = async () => {
            try {
                // Clean up any existing channel first
                if (gameUserChannelRef.current) {
                    await supabase.removeChannel(gameUserChannelRef.current);
                    gameUserChannelRef.current = null;
                }

                // Ensure we have proper auth before subscribing
                await supabase.realtime.setAuth();

                // Subscribe to personalized game-user channel for game updates
                const gameUserChannel = supabase.channel(`gu-${url_game_id}-${user_id}`, {
                    config: { private: true }
                });

                // Store the channel reference
                gameUserChannelRef.current = gameUserChannel;

                gameUserChannel
                    .on('broadcast', { event: 'animation_events' }, (payload) => {
                        handleAnimationMessage(payload.payload);
                    })
                    .subscribe((status, err) => {
                        if (status === 'SUBSCRIBED') {
                            animationChannelRetryInterval.current = 500; // Reset retry interval on success
                        } else if (status === 'CHANNEL_ERROR' || status === 'TIMED_OUT' || status === 'CLOSED') {
                            setTimeout(() => {
                                subscribeToGameAnimations().catch(console.error);
                                // Double the interval but cap at MAX_RETRY_INTERVAL
                                animationChannelRetryInterval.current = Math.min(animationChannelRetryInterval.current * 2, MAX_RETRY_INTERVAL);
                            }, animationChannelRetryInterval.current);
                        } else {
                        }
                    });
            } catch (error) {
                console.error('Error setting up game animation subscription:', error);
                setTimeout(() => {
                    subscribeToGameAnimations().catch(console.error);
                    // Double the interval but cap at MAX_RETRY_INTERVAL
                    animationChannelRetryInterval.current = Math.min(animationChannelRetryInterval.current * 2, MAX_RETRY_INTERVAL);
                }, animationChannelRetryInterval.current);
            }
        };

        subscribeToGameAnimations();
        
        // Cleanup function
        return () => {
            if (gameUserChannelRef.current) {
                // Use a timeout to avoid immediate cleanup race conditions
                setTimeout(async () => {
                    try {
                        if (gameUserChannelRef.current) {
                            await supabase.removeChannel(gameUserChannelRef.current);
                            gameUserChannelRef.current = null;
                        }
                    } catch (error) {
                        // Ignore cleanup errors - channel might already be closed
                        console.debug('Channel cleanup error (expected if WebSocket closed):', error);
                    }
                }, 100);
            }
        };
    }, [user_id, url_game_id]);

    // Handle animation messages from real-time channel
    const handleAnimationMessage = (message: any) => {
        if (message.events && Array.isArray(message.events)) {
            // Store the game ID for use during animations
            if (message.game?.id) {
                currentGameIdRef.current = message.game.id;
            }
            
            // Check for duplicate sequence_id FIRST (before checking optimistic events)
            const sequenceId = message.sequence_id || message.gameId + '-' + Date.now();
            
            if (processedSequenceIds.current.has(sequenceId)) {
                return;
            }
            
            // Also check event content as backup
            const eventsString = JSON.stringify(message.events);
            
            if (processedEventContent.current.has(eventsString)) {
                return;
            }
            
            // Mark as processed early to prevent race conditions
            processedSequenceIds.current.add(sequenceId);
            processedEventContent.current.add(eventsString);
            
            // Check EACH event individually to see if it was optimistically animated
            // Only skip the events that are optimistic, not the entire sequence
            const serverEvents = message.events;
            
            serverEvents.forEach((evt: any, idx: number) => {
            });
            
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
                    const cardEventString = JSON.stringify({
                        type: serverEvent.type,
                        card: card, // Check individual card
                        from_location: serverEvent.from_location,
                        to_location: serverEvent.to_location,
                        player_id: serverEvent.player_id
                    });
                    const timestamp = optimisticAnimations.current.get(cardEventString);
                    const isOptimistic = timestamp !== undefined;
                    const age = timestamp ? Date.now() - timestamp : null;
                    return isOptimistic;
                });
                
                if (allCardsOptimistic) {
                    optimisticEventIndices.push(eventIndex);
                    // Clear the optimistic animations since server confirmed them
                    serverEvent.cards.forEach((card: Card) => {
                        const cardKey = `${card.suit}-${card.value}`;
                        const cardEventString = JSON.stringify({
                            type: serverEvent.type,
                            card: card,
                            from_location: serverEvent.from_location,
                            to_location: serverEvent.to_location,
                            player_id: serverEvent.player_id
                        });
                        const timestamp = optimisticAnimations.current.get(cardEventString);
                        const wasDeleted = optimisticAnimations.current.delete(cardEventString);
                        
                        // Also clear position tracking since server confirmed the move
                        optimisticCardPositions.current.delete(cardKey);
                        
                        if (wasDeleted && timestamp) {
                            const age = Date.now() - timestamp;
                        }
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
            
            // Log which events have intermediate game states
            message.events.forEach((animEvent: ClientAnimationEvent, index: number) => {
                const hasGameState = !!animEvent.game_state;
                const stateInfo = hasGameState ? 
                    `table:${animEvent.game_state!.table_battles?.length || 0}, hands:${animEvent.game_state!.players?.map(p => p.hand?.length || 0).join(',')}` :
                    'NO STATE';
            });
            
            // Remove the duplicate checking logic that was moved to the top
            // Check for duplicate events using stringified content
            // const eventsString = JSON.stringify(message.events);
            
            // if (processedEventContent.current.has(eventsString)) {
            //     return;
            // }
            
            // processedEventContent.current.add(eventsString);
            
            // Also check sequence_id as backup (in case events are identical but from different sources)
            // const sequenceId = message.sequence_id || message.gameId + '-' + Date.now();
            // if (processedSequenceIds.current.has(sequenceId)) {
            //     return;
            // }
            // processedSequenceIds.current.add(sequenceId);
            
            // Clean up old sequence IDs to prevent memory leaks (keep only last 50)  
            // Event content is cleared after each sequence, so no cleanup needed there
            if (processedSequenceIds.current.size > 50) {
                const ids = Array.from(processedSequenceIds.current);
                processedSequenceIds.current = new Set(ids.slice(-25));
            }
            
            // CONFLICT DETECTION: Check if server events invalidate our optimistic animations
            const revertEvents: ClientAnimationEvent[] = [];
            
            // Get current displayed state (what the user sees with optimistic updates)
            const currentGame = url_game_id ? games[url_game_id] : null;
            
            console.log('\n🔍 ===== CONFLICT DETECTION ENTRY =====');
            console.log('🔍 Conflict detection entry:', {
                has_currentGame: !!currentGame,
                has_events: message.events?.length > 0,
                url_game_id,
                optimistic_tracking_size: optimisticAnimations.current.size
            });
            console.log(`📊 Optimistic pass state at entry: ${optimisticPassState.current ? `defender=${optimisticPassState.current.defender}, first_attacker=${optimisticPassState.current.first_attacker}` : 'NONE'}`);
            
            // Declare passIsInvalid here so it's in scope for revert queueing logic later
            let passIsInvalid = false;
            
            // Check for conflicts if we have ANY optimistic animations tracked
            // (Don't require currentGame since we check optimisticAnimations directly)
            if (message.events.length > 0 && optimisticAnimations.current.size > 0) {
                console.log('🔍 Checking for optimistic conflicts...');
                
                // Get the FINAL server game state (last event's state shows the end result)
                const lastEventWithState = [...message.events].reverse().find((evt: any) => evt.game_state);
                
                if (lastEventWithState && lastEventWithState.game_state) {
                    const serverState = lastEventWithState.game_state;
                    const myPlayerId = serverState.self?.player_id || user_id;
                    
                    // Check if server's final state already includes my optimistic cards
                    // If so, they were accepted! Don't revert.
                    const serverTableCards = serverState.table_battles?.flatMap((b: any) => 
                        b.defense ? [b.attack, b.defense] : [b.attack]
                    ) || [];
                    
                    console.log('  Server table (final state):', serverTableCards.map((c: Card) => `${c.suit}${c.value}`));
                    
                    // Find MY optimistic cards (attacks, covers, pickups)
                    const myOptimisticAttackCovers: Card[] = [];
                    const myOptimisticPickups: Card[] = [];
                    
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
                    
                    console.log('  My optimistic attacks/covers:', myOptimisticAttackCovers.map(c => `${c.suit}${c.value}`));
                    console.log('  My optimistic pickups:', myOptimisticPickups.map(c => `${c.suit}${c.value}`));
                    
                    // Check if server's final state already includes my optimistic attack/cover cards
                    // If so, server accepted them - don't revert!
                    const myOptimisticCardsAccepted = myOptimisticAttackCovers.filter(optCard => 
                        serverTableCards.some((serverCard: Card) => 
                            serverCard.suit === optCard.suit && serverCard.value === optCard.value
                        )
                    );
                    
                    if (myOptimisticCardsAccepted.length > 0) {
                        console.log(`  ✅ Server accepted optimistic cards:`, myOptimisticCardsAccepted.map(c => `${c.suit}${c.value}`));
                        console.log(`  ✅ No revert needed - server confirmed attacks`);
                        // Don't create revert events - server accepted them
                        // Continue to queue server events normally
                    }
                    
                    // ====== CHECK FOR OPTIMISTIC PASS CONFLICTS EARLY ======
                    // Do this BEFORE merging, so invalid pass cards don't get baked into states
                    if (optimisticPassState.current && message.events.length > 0) {
                        console.log('\n🔍 ===== CHECKING OPTIMISTIC PASS VALIDITY (EARLY) =====');
                        console.log(`📍 Current optimistic pass: defender → ${optimisticPassState.current.defender}`);
                        
                        const hasServerAttack = message.events.some((evt: any) => evt.type === 'attack_pass');
                        
                        if (hasServerAttack) {
                            console.log('  📋 Server sent attack event(s) - checking if pass is still valid');
                            
                            const nextDefenderId = optimisticPassState.current.defender;
                            const finalGameState = message.game || serverState;
                            const nextDefenderHandSize = finalGameState?.players?.[nextDefenderId]?.hand_length ?? 0;
                            
                            console.log(`  📊 Next defender (index ${nextDefenderId}): ${nextDefenderHandSize} cards`);
                            
                            const serverTableBattles = serverState?.table_battles || [];
                            const serverUncoveredAttacks = serverTableBattles.filter((b: any) => !b.defense).length;
                            
                            const serverAttackCards = message.events
                                .filter((evt: any) => evt.type === 'attack_pass')
                                .reduce((sum: number, evt: any) => sum + (evt.cards?.length || 0), 0);
                            
                            const optimisticPassCards = Array.from(optimisticAnimations.current.keys())
                                .filter(key => {
                                    try {
                                        const parsed = JSON.parse(key);
                                        return parsed.type === 'attack_pass' && 
                                               parsed.player_id === myPlayerId &&
                                               optimisticCardPositions.current.has(`${parsed.card.suit}-${parsed.card.value}`);
                                    } catch {
                                        return false;
                                    }
                                })
                                .length;
                            
                            const totalAttacksIfPassSucceeds = serverUncoveredAttacks + serverAttackCards + optimisticPassCards;
                            
                            console.log(`  📊 Attack count: ${serverUncoveredAttacks} uncovered + ${serverAttackCards} server + ${optimisticPassCards} optimistic = ${totalAttacksIfPassSucceeds} total`);
                            console.log(`  📊 Next defender can handle: ${nextDefenderHandSize} cards`);
                            
                            if (totalAttacksIfPassSucceeds > nextDefenderHandSize) {
                                console.log(`  🔴 PASS CONFLICT! ${totalAttacksIfPassSucceeds} attacks > ${nextDefenderHandSize} defender cards`);
                                console.log(`  🔴 Pass is now INVALID - will revert pass cards and defender change`);
                                passIsInvalid = true;
                                
                                // Collect pass cards to revert
                                const passCardsToRevert: Card[] = [];
                                optimisticAnimations.current.forEach((timestamp, key) => {
                                    try {
                                        const parsed = JSON.parse(key);
                                        if (parsed.type === 'attack_pass' && parsed.player_id === myPlayerId) {
                                            const cardId = `${parsed.card.suit}-${parsed.card.value}`;
                                            if (optimisticCardPositions.current.has(cardId)) {
                                                passCardsToRevert.push(parsed.card);
                                            }
                                        }
                                    } catch (e) {}
                                });
                                
                                if (passCardsToRevert.length > 0) {
                                    console.log(`  🔴 Creating revert for pass cards: ${passCardsToRevert.map(c => `${c.suit}${c.value}`).join(', ')}`);
                                    
                                    passCardsToRevert.forEach(card => {
                                        const cardId = `${card.suit}-${card.value}`;
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
                                    console.log(`  🔴 Clearing optimistic pass state: was defender=${optimisticPassState.current.defender}`);
                                    optimisticPassState.current = null;
                                    
                                    passCardsToRevert.forEach(card => {
                                        const cardEventString = JSON.stringify({
                                            type: 'attack_pass',
                                            card: card,
                                            from_location: 'hand',
                                            to_location: 'table',
                                            player_id: myPlayerId
                                        });
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
                            } else {
                                console.log(`  ✅ Pass is still VALID - next defender can handle all attacks`);
                            }
                        }
                        
                        console.log('===== END PASS VALIDITY CHECK (EARLY) =====\n');
                    }
                    
                    // ====== CHECK FOR OPTIMISTIC ATTACK + SERVER PASS CONFLICTS ======
                    // A pass is detected when the defender changes between states
                    // Check if server events contain a pass that invalidates optimistic attacks
                    const serverDefenderBefore = serverState?.defender;
                    const serverDefenderAfter = (message.game || serverState)?.defender;
                    const defenderChanged = serverDefenderBefore !== undefined && 
                                           serverDefenderAfter !== undefined && 
                                           serverDefenderBefore !== serverDefenderAfter;
                    
                    if (myOptimisticAttackCovers.length > 0 && defenderChanged) {
                        console.log('\n🔍 ===== CHECKING OPTIMISTIC ATTACK vs SERVER PASS =====');
                        console.log(`📍 My optimistic attacks: ${myOptimisticAttackCovers.map(c => `${c.suit}${c.value}`).join(', ')}`);
                        console.log(`📍 Defender changed: ${serverDefenderBefore} → ${serverDefenderAfter} (PASS detected)`);
                        
                        // Get the pass event (the attack_pass event that caused defender to change)
                        const serverPassEvent = message.events.find((evt: any) => evt.type === 'attack_pass');
                        
                        if (serverPassEvent) {
                            console.log('  📋 Server sent pass event - checking if optimistic attacks are still valid');
                            
                            const finalGameState = message.game || serverState;
                            const newDefenderId = serverDefenderAfter; // After pass
                            
                            console.log(`  📊 Pass changes defender: ${serverDefenderBefore} → ${newDefenderId}`);
                            
                            // Check 1: Did the pass make the attacker become the defender?
                            if (newDefenderId !== undefined) {
                                // Find my player index
                                const myPlayerIndex = finalGameState?.players?.findIndex((p: any) => 
                                    p.player_id === myPlayerId
                                );
                                
                                if (myPlayerIndex === newDefenderId) {
                                    console.log(`  🔴 ATTACK vs PASS CONFLICT! New defender is ME (index ${newDefenderId}) - can't attack myself!`);
                                    console.log(`  🔴 Reverting optimistic attacks`);
                                    
                                    // Revert all optimistic attacks
                                    myOptimisticAttackCovers.forEach(optCard => {
                                        const cardKey = `${optCard.suit}-${optCard.value}`;
                                        
                                        if (revertingCards.current.has(cardKey)) {
                                            console.log(`  Already reverting ${cardKey}, skipping`);
                                            return;
                                        }
                                        revertingCards.current.add(cardKey);
                                        
                                        const visualPosition = optimisticCardPositions.current.get(cardKey);
                                        const fromLocation = visualPosition?.location || 'table';
                                        
                                        console.log(`  Creating revert: ${cardKey} from ${fromLocation} → hand`);
                                        
                                        revertEvents.push({
                                            type: 'revert',
                                            cards: [optCard],
                                            from_location: fromLocation as any,
                                            to_location: 'hand',
                                            player_id: myPlayerId,
                                            is_revert: true,
                                            game_state: null as any
                                        });
                                        
                                        const cardEventString = JSON.stringify({
                                            type: 'attack_pass',
                                            card: optCard,
                                            from_location: 'hand',
                                            to_location: 'table',
                                            player_id: myPlayerId
                                        });
                                        optimisticAnimations.current.delete(cardEventString);
                                    });
                                    
                                    // Remove from merge list
                                    myOptimisticAttackCovers.length = 0;
                                }
                            }
                            
                            // Check 2: Does the new defender have exactly enough cards for the table?
                            if (myOptimisticAttackCovers.length > 0 && newDefenderId !== undefined) {
                                const newDefenderHandSize = finalGameState?.players?.[newDefenderId]?.hand_length ?? 0;
                                const serverPassCards = serverPassEvent.cards?.length || 0;
                                
                                // Count uncovered attacks on server's table (before pass)
                                const serverTableBattles = serverState?.table_battles || [];
                                const serverUncoveredAttacks = serverTableBattles.filter((b: any) => !b.defense).length;
                                
                                // Total attacks the new defender will face = uncovered + pass cards
                                const totalAttacksAfterPass = serverUncoveredAttacks + serverPassCards;
                                
                                console.log(`  📊 New defender (index ${newDefenderId}): ${newDefenderHandSize} cards`);
                                console.log(`  📊 Attacks after pass: ${serverUncoveredAttacks} uncovered + ${serverPassCards} pass = ${totalAttacksAfterPass} total`);
                                console.log(`  📊 Optimistic attacks: ${myOptimisticAttackCovers.length}`);
                                
                                // If table is full (new defender has exactly enough cards), no more attacks allowed
                                if (totalAttacksAfterPass >= newDefenderHandSize) {
                                    console.log(`  🔴 ATTACK vs PASS CONFLICT! Table is FULL (${totalAttacksAfterPass} >= ${newDefenderHandSize}) - no room for optimistic attacks!`);
                                    console.log(`  🔴 Reverting optimistic attacks`);
                                    
                                    // Revert all optimistic attacks
                                    myOptimisticAttackCovers.forEach(optCard => {
                                        const cardKey = `${optCard.suit}-${optCard.value}`;
                                        
                                        if (revertingCards.current.has(cardKey)) {
                                            console.log(`  Already reverting ${cardKey}, skipping`);
                                            return;
                                        }
                                        revertingCards.current.add(cardKey);
                                        
                                        const visualPosition = optimisticCardPositions.current.get(cardKey);
                                        const fromLocation = visualPosition?.location || 'table';
                                        
                                        console.log(`  Creating revert: ${cardKey} from ${fromLocation} → hand`);
                                        
                                        revertEvents.push({
                                            type: 'revert',
                                            cards: [optCard],
                                            from_location: fromLocation as any,
                                            to_location: 'hand',
                                            player_id: myPlayerId,
                                            is_revert: true,
                                            game_state: null as any
                                        });
                                        
                                        const cardEventString = JSON.stringify({
                                            type: 'attack_pass',
                                            card: optCard,
                                            from_location: 'hand',
                                            to_location: 'table',
                                            player_id: myPlayerId
                                        });
                                        optimisticAnimations.current.delete(cardEventString);
                                    });
                                    
                                    // Remove from merge list
                                    myOptimisticAttackCovers.length = 0;
                                } else {
                                    console.log(`  ✅ Optimistic attacks are still VALID - new defender can handle all attacks`);
                                }
                            }
                        }
                        
                        console.log('===== END ATTACK vs PASS CHECK =====\n');
                    }
                    
                    // Handle optimistic pickup conflicts
                    if (myOptimisticPickups.length > 0) {
                        console.log(`  🔍 Checking if optimistic pickup conflicts with server state...`);
                        
                        // Two scenarios:
                        // 1. Server shows cards still on table (attack came in) - revert to table
                        // 2. Server sends cards_to_trash/discard (good was played) - revert to table before trash
                        
                        const hasCardsToTrash = message.events.some((evt: any) => 
                            evt.type === 'cards_to_trash' || evt.type === 'discard'
                        );
                        
                        let pickupCardsToRevert: Card[] = [];
                        
                        if (hasCardsToTrash) {
                            // Good was played - ALL optimistic pickups need to be reverted
                            console.log(`  🔴 PICKUP vs GOOD CONFLICT! Server sent cards_to_trash, reverting all pickups`);
                            pickupCardsToRevert = myOptimisticPickups;
                        } else if (serverTableCards.length > 0) {
                            // Attack came in - only revert cards that are still on table
                            pickupCardsToRevert = myOptimisticPickups.filter(pickupCard => 
                                serverTableCards.some((serverCard: Card) => 
                                    serverCard.suit === pickupCard.suit && serverCard.value === pickupCard.value
                                )
                            );
                            if (pickupCardsToRevert.length > 0) {
                                console.log(`  🔴 PICKUP vs ATTACK CONFLICT! Cards still on server table:`, pickupCardsToRevert.map(c => `${c.suit}${c.value}`));
                            }
                        }
                        
                        if (pickupCardsToRevert.length > 0) {
                            console.log(`  🔴 Creating single multi-card revert animation for ${pickupCardsToRevert.length} card(s)`);
                            
                            // Mark all cards as reverting and clear tracking
                            pickupCardsToRevert.forEach(card => {
                                const cardKey = `${card.suit}-${card.value}`;
                                revertingCards.current.add(cardKey);
                                
                                // Clear tracking
                                const cardEventString = JSON.stringify({
                                    type: 'pickup',
                                    card: card,
                                    from_location: 'table',
                                    to_location: 'hand',
                                    player_id: myPlayerId
                                });
                                optimisticAnimations.current.delete(cardEventString);
                            });
                            
                            // Create SINGLE revert event with ALL cards
                            console.log(`  Creating pickup revert: ${pickupCardsToRevert.map(c => `${c.suit}${c.value}`).join(', ')} from hand → table`);
                            
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
                        // Server didn't include our optimistic cards yet
                        
                        // SPECIAL CASE: If server sent pickup/cards_to_trash, table is cleared
                        // Our optimistic attacks were too slow - revert them immediately
                        console.log(`  📋 Checking for table-clear events in ${message.events.length} events:`);
                        message.events.forEach((evt: any, idx: number) => {
                            console.log(`    Event ${idx}: type=${evt.type}, cards=${evt.cards?.map((c: Card) => `${c.suit}${c.value}`)}`);
                        });
                        
                        const hasTableClearEvent = message.events.some((evt: any) => 
                            evt.type === 'pickup' || evt.type === 'cards_to_trash'
                        );
                        console.log(`  hasTableClearEvent: ${hasTableClearEvent}`);
                        
                        if (hasTableClearEvent) {
                            console.log(`  🔴 Server sent table-clear event (pickup/cards_to_trash) - reverting optimistic cards`);
                            console.log(`  🔴 Optimistic cards to revert:`, myOptimisticAttackCovers.map(c => `${c.suit}${c.value}`));
                            
                            myOptimisticAttackCovers.forEach(optCard => {
                                const cardKey = `${optCard.suit}-${optCard.value}`;
                                
                                // Check if already reverting
                                if (revertingCards.current.has(cardKey)) {
                                    console.log(`  Already reverting ${cardKey}, skipping`);
                                    return;
                                }
                                revertingCards.current.add(cardKey);
                                
                                // Get visual position
                                const visualPosition = optimisticCardPositions.current.get(cardKey);
                                const fromLocation = visualPosition?.location || 'table';
                                
                                console.log(`  Creating revert: ${cardKey} from ${fromLocation} → hand`);
                                
                                revertEvents.push({
                                    type: 'revert',
                                    cards: [optCard],
                                    from_location: fromLocation as any,
                                    to_location: 'hand',
                                    player_id: myPlayerId,
                                    is_revert: true,
                                    game_state: null as any // Will be set later in revert queueing logic
                                });
                                
                                // Clear tracking
                                const cardEventString = JSON.stringify({
                                    type: 'attack_pass',
                                    card: optCard,
                                    from_location: 'hand',
                                    to_location: 'table',
                                    player_id: myPlayerId
                                });
                                optimisticAnimations.current.delete(cardEventString);
                            });
                            
                            // CRITICAL: Don't merge optimistic cards into pickup/cards_to_trash events!
                            // The table is already cleared on the server, optimistic cards were too slow
                            console.log(`  ⚠️  Skipping merge for table-clear events`);
                        } else {
                            // Check if we should revert based on defender capacity
                        
                        console.log('🔍 DEBUG: Finding defender hand size...');
                        
                        // Try multiple sources for defender hand size
                        const currentGame = url_game_id ? games[url_game_id] : undefined;
                        const finalGameState = message.game || serverState;
                        const defenderId = finalGameState?.defender;
                        
                        console.log('  Sources available:', JSON.stringify({
                            has_currentGame: !!currentGame,
                            has_messageGame: !!message.game,
                            has_serverState: !!serverState,
                            defenderId
                        }));
                        
                        // Check what's in each source
                        if (currentGame && currentGame.defender !== undefined) {
                            const clientDefenderHandSize = currentGame.players?.[currentGame.defender]?.hand_length ?? 0;
                            console.log(`  Client game (games[${url_game_id}]): defender has ${clientDefenderHandSize} cards`);
                        }
                        
                        if (message.game && message.game.defender !== undefined) {
                            const msgDefenderHandSize = message.game.players?.[message.game.defender]?.hand_length ?? 0;
                            console.log(`  message.game: defender has ${msgDefenderHandSize} cards`);
                            console.log(`  message.game.players:`, JSON.stringify(message.game.players?.map((p: any) => ({ 
                                id: p.player_id?.slice(0,8), 
                                handSize: p.hand_length ?? 'NO_HAND_LENGTH',
                                handCards: p.hand?.map((c: Card) => `${c.suit}${c.value}`) ?? 'NO_HAND',
                                isDefender: message.game.defender === message.game.players.indexOf(p)
                            }))));
                        }
                        
                        // Use client game state if available (most accurate), otherwise fall back to message.game
                        const defenderHandSize = (currentGame && currentGame.defender !== undefined)
                            ? (currentGame.players?.[currentGame.defender]?.hand_length ?? 0)
                            : (finalGameState?.defender !== undefined ? (finalGameState.players?.[finalGameState.defender]?.hand_length ?? 0) : 0);
                        
                        const finalUncoveredAttacks = finalGameState?.table_battles?.filter((b: any) => !b.defense).length ?? 0;
                        
                        console.log(`  ✅ USING: ${finalUncoveredAttacks} uncovered, defender has ${defenderHandSize} cards (from ${currentGame ? 'client game' : 'message.game'})`);
                        console.log(`  My optimistic attacks: ${myOptimisticAttackCovers.length}`);
                        
                        // Simple capacity check: Can defender handle all attacks?
                        const totalAttacks = finalUncoveredAttacks + myOptimisticAttackCovers.length;
                        
                        console.log(`  Total attacks if all accepted: ${totalAttacks}`);
                        
                        if (totalAttacks > defenderHandSize) {
                            console.log(`🔴 CONFLICT DETECTED! ${totalAttacks} total attacks > ${defenderHandSize} defender cards`);
                            console.log('🔴 Creating revert animations for:', myOptimisticAttackCovers.map(c => `${c.suit}${c.value}`));
                                
                                // Create revert animation for my invalid optimistic cards
                                myOptimisticAttackCovers.forEach((card: Card) => {
                                const cardKey = `${card.suit}-${card.value}`;
                                
                                if (revertingCards.current.has(cardKey)) {
                                    console.log(`  Skipping ${cardKey} - already reverting`);
                                    return;
                                }
                                
                                revertingCards.current.add(cardKey);
                                
                                // Get where this card currently is visually
                                const visualPosition = optimisticCardPositions.current.get(cardKey);
                                const fromLocation = visualPosition?.location || 'table';
                                
                                console.log(`  Creating revert: ${cardKey} from ${fromLocation} → hand`);
                                
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
                                const cardEventString = JSON.stringify({
                                    type: 'attack_pass',
                                    card: card,
                                    from_location: 'hand',
                                    to_location: 'table',
                                    player_id: myPlayerId
                                });
                                    optimisticAnimations.current.delete(cardEventString);
                                });
                        } else {
                            console.log(`  ✅ Defender can handle all attacks (${totalAttacks} <= ${defenderHandSize})`);
                            console.log(`  ✅ Merging optimistic cards into server events AND final state to maintain visual consistency`);
                            
                            // Get my player ID
                            const myPlayerId = serverState.self?.player_id || user_id;
                            
                            // Merge optimistic cards into ALL game states (events + final)
                            const statesToMerge = [
                                ...message.events.map((evt: any) => evt.game_state).filter(Boolean),
                                message.game
                            ].filter(Boolean);
                            
                            statesToMerge.forEach((state: any, stateIdx: number) => {
                                if (state && state.table_battles) {
                                    console.log(`  🔄 Merging into state ${stateIdx}:`);
                                    console.log(`    📊 State BEFORE any changes: defender=${state.defender}, first_attacker=${state.first_attacker}`);
                                    
                                    // FIRST: Preserve optimistic pass state if present
                                    if (optimisticPassState.current) {
                                        console.log(`    🎯 PRESERVING optimistic pass: ${state.defender} → ${optimisticPassState.current.defender}, ${state.first_attacker} → ${optimisticPassState.current.first_attacker}`);
                                        state.defender = optimisticPassState.current.defender;
                                        state.first_attacker = optimisticPassState.current.first_attacker;
                                        console.log(`    ✅ State AFTER preservation: defender=${state.defender}, first_attacker=${state.first_attacker}`);
                                    }
                                    
                                    // Log before state
                                    const tableBefore = state.table_battles.map((b: any) => 
                                        `${b.attack.suit}${b.attack.value}${b.defense ? `/${b.defense.suit}${b.defense.value}` : ''}`
                                    );
                                    const myPlayer = state.players?.find((p: any) => p.player_id === myPlayerId) || state.self;
                                    const handBefore = myPlayer?.hand?.map((c: Card) => `${c.suit}${c.value}`) ?? [];
                                    
                                    console.log(`    Before: table=${JSON.stringify(tableBefore)}, myHand=${JSON.stringify(handBefore)}`);
                                    
                                    myOptimisticAttackCovers.forEach((optCard: Card) => {
                                        const alreadyPresent = state.table_battles.some((b: any) => 
                                            (b.attack.suit === optCard.suit && b.attack.value === optCard.value) ||
                                            (b.defense && b.defense.suit === optCard.suit && b.defense.value === optCard.value)
                                        );
                                        if (!alreadyPresent) {
                                            const cardKey = `${optCard.suit}-${optCard.value}`;
                                            const positionInfo = optimisticCardPositions.current.get(cardKey);
                                            
                                            // Check if this is a cover (has target_card info)
                                            if (positionInfo?.target_card) {
                                                // This is a cover - add as defense to the correct battle
                                                const targetCard = positionInfo.target_card;
                                                const battleIdx = state.table_battles.findIndex((b: any) => 
                                                    b.attack.suit === targetCard.suit && b.attack.value === targetCard.value
                                                );
                                                if (battleIdx >= 0 && !state.table_battles[battleIdx].defense) {
                                                    console.log(`    Adding optimistic cover ${optCard.suit}${optCard.value} to battle ${battleIdx} (covering ${targetCard.suit}${targetCard.value})`);
                                                    state.table_battles[battleIdx].defense = optCard;
                                                } else {
                                                    console.log(`    ⚠️ Could not find battle for cover ${optCard.suit}${optCard.value} (target ${targetCard.suit}${targetCard.value})`);
                                                }
                                            } else {
                                                // This is an attack - add as new battle
                                                console.log(`    Adding optimistic attack ${optCard.suit}${optCard.value} to table_battles`);
                                                state.table_battles.push({
                                                    attack: optCard,
                                                    defense: null
                                                });
                                            }
                                        }
                                        
                                        // CRITICAL: Also remove this card from my hand in this state!
                                        // Find my player in this state
                                        if (state.self?.player_id === myPlayerId && state.self.hand) {
                                            const handLengthBefore = state.self.hand.length;
                                            state.self.hand = state.self.hand.filter((c: Card) => 
                                                !(c.suit === optCard.suit && c.value === optCard.value)
                                            );
                                            if (state.self.hand.length < handLengthBefore) {
                                                console.log(`    Removed optimistic ${optCard.suit}${optCard.value} from self.hand`);
                                            }
                                        }
                                        
                                        // Also check players array
                                        if (state.players) {
                                            state.players.forEach((p: any, idx: number) => {
                                                if (p.player_id === myPlayerId && p.hand) {
                                                    const handLengthBefore = p.hand.length;
                                                    p.hand = p.hand.filter((c: Card) => 
                                                        !(c.suit === optCard.suit && c.value === optCard.value)
                                                    );
                                                    if (p.hand.length < handLengthBefore) {
                                                        console.log(`    Removed optimistic ${optCard.suit}${optCard.value} from players[${idx}].hand`);
                                                    }
                                                    // Update hand_length too
                                                    p.hand_length = p.hand.length;
                                                }
                                            });
                                        }
                                    });
                                    
                                    // Log after state
                                    const tableAfter = state.table_battles.map((b: any) => 
                                        `${b.attack.suit}${b.attack.value}${b.defense ? `/${b.defense.suit}${b.defense.value}` : ''}`
                                    );
                                    const myPlayerAfter = state.players?.find((p: any) => p.player_id === myPlayerId) || state.self;
                                    const handAfter = myPlayerAfter?.hand?.map((c: Card) => `${c.suit}${c.value}`) ?? [];
                                    
                                    console.log(`    After: table=${JSON.stringify(tableAfter)}, myHand=${JSON.stringify(handAfter)}`);
                                }
                            });
                        }
                        } // End of else block for capacity check
                    }
                }
            }
            
            // Store the completion callback to update final game state
            pendingCompletionCallbackRef.current = () => {
                if (message.game) {
                    console.log(`\n📊 ===== FINAL STATE UPDATE =====`);
                    console.log(`📊 FINAL STATE BEFORE ANY CHANGES: defender=${message.game.defender}, first_attacker=${message.game.first_attacker}`);
                    console.log(`📊 Optimistic pass state: ${optimisticPassState.current ? `defender=${optimisticPassState.current.defender}, first_attacker=${optimisticPassState.current.first_attacker}` : 'NONE'}`);
                    
                    // Check if server confirmed optimistic pass BEFORE modifying
                    let serverConfirmedPass = false;
                    if (optimisticPassState.current) {
                        serverConfirmedPass = 
                            message.game.defender === optimisticPassState.current.defender &&
                            message.game.first_attacker === optimisticPassState.current.first_attacker;
                        
                        if (serverConfirmedPass) {
                            console.log(`  ✅ Server CONFIRMED optimistic pass (defender: ${optimisticPassState.current.defender}, first_attacker: ${optimisticPassState.current.first_attacker})`);
                            console.log(`  ✅ Clearing optimistic pass tracking`);
                            optimisticPassState.current = null;
                        } else {
                            console.log(`🎯 Server NOT confirmed - PRESERVING optimistic pass: defender ${message.game.defender} → ${optimisticPassState.current.defender}, first_attacker ${message.game.first_attacker} → ${optimisticPassState.current.first_attacker}`);
                            message.game.defender = optimisticPassState.current.defender;
                            message.game.first_attacker = optimisticPassState.current.first_attacker;
                            console.log(`✅ FINAL STATE AFTER PRESERVATION: defender=${message.game.defender}, first_attacker=${message.game.first_attacker}`);
                        }
                    } else {
                        console.log(`⚠️ NO optimistic pass state to check`);
                    }
                    
                    console.log(`🎮 Calling updateGameState with: defender=${message.game.defender}, first_attacker=${message.game.first_attacker}`);
                    updateGameState(message.game.id, message.game);
                    console.log(`===== END FINAL STATE UPDATE =====\n`);
                }
            };
            remainingSequenceEventsRef.current = message.events.length + revertEvents.length;
            
            // If there are revert events, we need to keep invalid cards on table until revert animates
            if (revertEvents.length > 0) {
                console.log(`\n🔴 ===== QUEUEING REVERT ANIMATIONS =====`);
                console.log(`🔴 ${revertEvents.length} revert animation(s) to queue`);
                
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
                        console.log(`  Using magic_transition state for pickup reverts`);
                    } else if (pickupEvent?.cards) {
                        // Reconstruct state with cards on table (before pickup)
                        baseState = serverStateForRevert ? JSON.parse(JSON.stringify(serverStateForRevert)) : null;
                        if (baseState && pickupEvent.cards) {
                            // Put the cards back on the table as uncovered attacks
                            baseState.table_battles = pickupEvent.cards.map((card: Card) => ({
                                attack: card,
                                defense: null
                            }));
                            console.log(`  Reconstructed table for pickup reverts:`, pickupEvent.cards.map((c: Card) => `${c.suit}${c.value}`));
                        }
                    } else {
                        baseState = currentGame || serverStateForRevert;
                    }
                } else {
                    baseState = currentGame || serverStateForRevert;
                }
                
                const stateWithOptimistic = baseState ? JSON.parse(JSON.stringify(baseState)) : null;
                
                // Check if we have pass reverts - they need original defender value
                const hasPassReverts = revertEvents.some(rev => 
                    rev.to_location === 'hand' && 
                    passIsInvalid // We detected an invalid pass earlier
                );
                
                if (hasPassReverts && stateWithOptimistic && serverStateForRevert) {
                    // For pass reverts, use the SERVER's defender value (original before pass)
                    console.log(`  📍 Pass revert detected - restoring original defender: ${stateWithOptimistic.defender} → ${serverStateForRevert.defender}`);
                    stateWithOptimistic.defender = serverStateForRevert.defender;
                    stateWithOptimistic.first_attacker = serverStateForRevert.first_attacker;
                }
                
                console.log(`  Base state for revert: ${hasPickupRevertsForState ? 'pickup reverts' : hasPassReverts ? 'pass revert' : 'attack conflict'}`);
                if (stateWithOptimistic) {
                    const baseTableCards = stateWithOptimistic.table_battles?.flatMap((b: any) => 
                        b.defense ? [b.attack, b.defense] : [b.attack]
                    ) || [];
                    console.log(`  Base table before cleaning:`, baseTableCards.map((c: Card) => `${c.suit}${c.value}`));
                    console.log(`  Defender in revert state: ${stateWithOptimistic.defender}, first_attacker: ${stateWithOptimistic.first_attacker}`);
                }
                
                if (stateWithOptimistic) {
                    // IMPORTANT: Remove BOTH optimistic cards AND cards that will be animated
                    // This prevents the "transform" issue where invalid card becomes valid card
                    
                    const revertCardKeys = new Set(
                        revertEvents.flatMap(evt => evt.cards?.map(c => `${c.suit}-${c.value}`) || [])
                    );
                    
                    // For attack conflicts: remove cards that will be animated (valid attacks)
                    const serverAttackCards = new Set(
                        message.events
                            .filter((evt: any) => evt.type === 'attack_pass' && evt.from_location === 'hand')
                            .flatMap((evt: any) => evt.cards?.map((c: Card) => `${c.suit}-${c.value}`) || [])
                    );
                    
                    // Check if we have pickup reverts (hand → table) vs attack reverts (table → hand)
                    const hasPickupRevertsForClean = revertEvents.some(rev => rev.to_location === 'table');
                    const hasAttackReverts = revertEvents.some(rev => rev.to_location === 'hand');
                    const hasPickupEventForClean = message.events.some((evt: any) => evt.type === 'pickup' || evt.type === 'cards_to_trash');
                    
                    console.log(`  Removing from revert game_state:`, {
                        reverting: Array.from(revertCardKeys),
                        serverAttacks: Array.from(serverAttackCards),
                        hasPickupReverts: hasPickupRevertsForClean,
                        hasAttackReverts,
                        hasPickupEvent: hasPickupEventForClean
                    });
                    
                    if (hasPickupRevertsForClean) {
                        // PICKUP REVERT SCENARIO (hand → table): 
                        // State should show reverted cards on table, but NOT server attack cards that will animate
                        stateWithOptimistic.table_battles = (stateWithOptimistic.table_battles || []).filter((b: any) => {
                            const attackKey = `${b.attack.suit}-${b.attack.value}`;
                            const defenseKey = b.defense ? `${b.defense.suit}-${b.defense.value}` : null;
                            
                            // Remove server attack cards that will animate (they shouldn't appear yet)
                            const removeAttack = serverAttackCards.has(attackKey);
                            const removeDefense = defenseKey && serverAttackCards.has(defenseKey);
                            
                            return !removeAttack && !removeDefense;
                        });
                        console.log(`  Removed server attack cards from pickup revert state (will animate separately)`);
                    } else if (hasPickupEventForClean && hasAttackReverts) {
                        // ATTACK REVERT + PICKUP SCENARIO: Only remove reverting cards, keep everything else
                        // (Cards to be picked up need to stay on table for pickup animation)
                        stateWithOptimistic.table_battles = (stateWithOptimistic.table_battles || []).filter((b: any) => {
                            const attackKey = `${b.attack.suit}-${b.attack.value}`;
                            const defenseKey = b.defense ? `${b.defense.suit}-${b.defense.value}` : null;
                            
                            // Only remove reverting cards
                            const removeAttack = revertCardKeys.has(attackKey);
                            const removeDefense = defenseKey && revertCardKeys.has(defenseKey);
                            
                            return !removeAttack && !removeDefense;
                        });
                    } else {
                        // ATTACK CONFLICT SCENARIO: Remove both reverting AND valid attacks that will animate
                        stateWithOptimistic.table_battles = (stateWithOptimistic.table_battles || []).filter((b: any) => {
                            const attackKey = `${b.attack.suit}-${b.attack.value}`;
                            const defenseKey = b.defense ? `${b.defense.suit}-${b.defense.value}` : null;
                            
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
                            console.log(`  📍 Added ${passRevertCards.length} card(s) back to hand for pass revert`);
                        }
                    }
                    
                    const finalTableCards = stateWithOptimistic.table_battles?.flatMap((b: any) => 
                        b.defense ? [b.attack, b.defense] : [b.attack]
                    ) || [];
                    console.log(`  Revert game_state table (cleaned):`, finalTableCards.map((c: Card) => `${c.suit}${c.value}`));
                    if (hasPassReverts) {
                        const handCards = stateWithOptimistic.self?.hand?.map((c: Card) => `${c.suit}${c.value}`) || [];
                        console.log(`  Revert game_state hand:`, handCards);
                    }
                }
                
                revertEvents.forEach((revertEvent, idx) => {
                    revertEvent.game_state = stateWithOptimistic as any;
                    console.log(`  Revert ${idx}: ${revertEvent.cards?.map(c => `${c.suit}${c.value}`)} from ${revertEvent.from_location} → ${revertEvent.to_location}`);
                });
                
                console.log(`  Server events: ${message.events.length} total`);
                message.events.forEach((evt: any, idx: number) => {
                    console.log(`    Event ${idx}: ${evt.type} - ${evt.cards?.map((c: Card) => `${c.suit}${c.value}`)?.join(',') || 'no cards'}`);
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
                    console.log(`  Found pickup reverts + magic_transition at index ${firstMagicTransitionIndex} - inserting reverts before it`);
                    
                    // For optimistic pickup + server good: revert ALL cards back to table first
                    const eventsBeforeMagic = message.events.slice(0, firstMagicTransitionIndex);
                    const restEvents = message.events.slice(firstMagicTransitionIndex);
                    
                    const queueOrder = [
                        ...eventsBeforeMagic,
                        ...revertEvents,    // Revert pickups back to table first
                        ...restEvents       // Then magic_transition + cards_to_trash
                    ];
                    
                    console.log(`  📋 Final queue order (${queueOrder.length} events):`);
                    queueOrder.forEach((evt, idx) => {
                        console.log(`    ${idx}: ${evt.type} ${evt.is_revert ? '🔴' : ''} - ${evt.cards?.map((c: Card) => `${c.suit}${c.value}`)?.join(',') || 'no cards'}`);
                    });
                    
                    setAnimationQueue(prev => [...prev, ...queueOrder]);
                } else if (firstAttackIndex >= 0) {
                    console.log(`  Found valid attack at index ${firstAttackIndex} - inserting reverts before it`);
                    
                    // Queue reverts IMMEDIATELY before the valid attack for parallel visual effect
                    const eventsBeforeAttack = message.events.slice(0, firstAttackIndex);
                    const restEvents = message.events.slice(firstAttackIndex);
                    
                    const queueOrder = [
                        ...eventsBeforeAttack,
                        ...revertEvents,    // Revert animates
                        ...restEvents       // Valid attack animates right after (looks parallel)
                    ];
                    
                    console.log(`  📋 Final queue order (${queueOrder.length} events):`);
                    queueOrder.forEach((evt, idx) => {
                        console.log(`    ${idx}: ${evt.type} ${evt.is_revert ? '🔴' : ''} - ${evt.cards?.map((c: Card) => `${c.suit}${c.value}`)?.join(',') || 'no cards'}`);
                    });
                    
                    setAnimationQueue(prev => [...prev, ...queueOrder]);
                } else if (firstPickupIndex >= 0) {
                    console.log(`  Found pickup/clear at index ${firstPickupIndex} - inserting reverts before it`);
                    
                    // Queue reverts before the pickup so card goes back to hand first
                    const eventsBeforePickup = message.events.slice(0, firstPickupIndex);
                    const restEvents = message.events.slice(firstPickupIndex);
                    
                    const queueOrder = [
                        ...eventsBeforePickup,
                        ...revertEvents,    // Revert animates first
                        ...restEvents       // Then pickup animates
                    ];
                    
                    console.log(`  📋 Final queue order (${queueOrder.length} events):`);
                    queueOrder.forEach((evt, idx) => {
                        console.log(`    ${idx}: ${evt.type} ${evt.is_revert ? '🔴' : ''} - ${evt.cards?.map((c: Card) => `${c.suit}${c.value}`)?.join(',') || 'no cards'}`);
                    });
                    
                    setAnimationQueue(prev => [...prev, ...queueOrder]);
                } else {
                    console.log(`  No attack/pickup event found - queueing reverts then all server events`);
                    setAnimationQueue(prev => [...prev, ...revertEvents, ...message.events]);
                }
                console.log(`🔴 ===== END REVERT QUEUEING =====\n`);
            } else {
                console.log(`  No reverts needed - queueing ${message.events.length} server events normally`);
                console.log(`📊 Optimistic pass state when queueing: ${optimisticPassState.current ? `defender=${optimisticPassState.current.defender}, first_attacker=${optimisticPassState.current.first_attacker}` : 'NONE'}`);
            // Queue all events from the sequence
            setAnimationQueue(prev => [...prev, ...message.events]);
            }
        }
    };

    // Helper function to create a unique card key
    const getCardKey = (card: Card, playerId?: string) => {
        return `${card.suit}-${card.value}-${playerId || 'global'}`;
    };

    // Process the animation queue
    const processAnimationQueue = useCallback(() => {
        if (animationQueueRef.current.length === 0) {
            console.log('📭 Animation queue empty');
            setIsAnimating(false);
            setCurrentAnimation(null);
            
            // Clear processed event content when queue is empty (allows future legitimate duplicates)
            if (processedEventContent.current.size > 0) {
                processedEventContent.current.clear();
            }
            
            // Check if we have a pending completion callback and we've finished the sequence
            if (pendingCompletionCallbackRef.current && remainingSequenceEventsRef.current === 0) {
                console.log('✅ Sequence complete - applying final game state');
                const callback = pendingCompletionCallbackRef.current;
                pendingCompletionCallbackRef.current = null;
                remainingSequenceEventsRef.current = 0;
                callback();
            }
            
            return;
        }

        const nextAnimation = animationQueueRef.current[0];
        console.log(`\n▶️  ===== PROCESSING ANIMATION =====`);
        console.log(`▶️  NEXT ANIMATION: ${nextAnimation.type}`, {
            cards: nextAnimation.cards?.map(c => `${c.suit}${c.value}`),
            from: nextAnimation.from_location,
            to: nextAnimation.to_location,
            player: nextAnimation.player_id?.substring(0, 8),
            is_revert: nextAnimation.is_revert,
            has_game_state: !!nextAnimation.game_state,
            queue_remaining: animationQueueRef.current.length - 1
        });
        console.log(`📊 Current optimistic pass state: ${optimisticPassState.current ? `defender=${optimisticPassState.current.defender}, first_attacker=${optimisticPassState.current.first_attacker}` : 'NONE'}`);
        
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

        // Start tracking cards in this animation (simplified - CSS handles the actual animation)
        if (nextAnimation.cards && nextAnimation.cards.length > 0) {
            setAnimatingCards(prev => {
                const newAnimatingCards = new Map(prev);
                
                nextAnimation.cards!.forEach(card => {
                    const cardKey = getCardKey(card, nextAnimation.player_id);
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
                console.log(`📊 STATE BEFORE ANY CHANGES: defender=${nextAnimation.game_state.defender}, first_attacker=${nextAnimation.game_state.first_attacker}`);
                
                // If we have an optimistic pass, preserve defender/first_attacker
                if (optimisticPassState.current) {
                    console.log(`🎯 PRESERVING optimistic pass before applying: ${nextAnimation.game_state.defender} → ${optimisticPassState.current.defender}, ${nextAnimation.game_state.first_attacker} → ${optimisticPassState.current.first_attacker}`);
                    nextAnimation.game_state.defender = optimisticPassState.current.defender;
                    nextAnimation.game_state.first_attacker = optimisticPassState.current.first_attacker;
                    console.log(`✅ STATE AFTER PRESERVATION: defender=${nextAnimation.game_state.defender}, first_attacker=${nextAnimation.game_state.first_attacker}`);
                } else {
                    console.log(`⚠️ NO optimistic pass state to preserve`);
                }
                
                const tableCards = nextAnimation.game_state.table_battles?.flatMap((b: any) => 
                    b.defense ? [b.attack, b.defense] : [b.attack]
                ) || [];
                console.log(`🎮 APPLYING GAME STATE after ${nextAnimation.type}:`, {
                    table_cards: tableCards.map((c: Card) => `${c.suit}${c.value}`),
                    table_count: tableCards.length,
                    defender: nextAnimation.game_state.defender,
                    first_attacker: nextAnimation.game_state.first_attacker
                });
                    updateGameState(currentGameIdRef.current, nextAnimation.game_state);
            } else {
                console.log(`⏭️  No game state to apply for ${nextAnimation.type}`);
            }
            
            // Remove cards from animating state
            if (nextAnimation.cards) {
                setAnimatingCards(prev => {
                    const updated = new Map(prev);
                    nextAnimation.cards!.forEach(card => {
                        const cardKey = getCardKey(card, nextAnimation.player_id);
                        updated.delete(cardKey);
                    });
                    return updated;
                });
                
                // If this was a revert animation, clear the reverting and position tracking
                if (nextAnimation.type === 'revert') {
                    nextAnimation.cards.forEach(card => {
                        const cardKey = `${card.suit}-${card.value}`;
                        revertingCards.current.delete(cardKey);
                        optimisticCardPositions.current.delete(cardKey);
                        console.log(`  ✅ Cleared reverting flag and position tracking for ${cardKey}`);
                    });
                }
                
                // Log current tracking state
                if (optimisticCardPositions.current.size > 0) {
                    console.log(`  📍 Still tracking positions:`, Array.from(optimisticCardPositions.current.entries()));
                }
            }

            // Decrement remaining sequence events count if we're tracking a sequence
            if (pendingCompletionCallbackRef.current && remainingSequenceEventsRef.current > 0) {
                remainingSequenceEventsRef.current--;
            }
            
            // Process next animation after a short delay
            setTimeout(processAnimationQueue, 100);
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

    // Queue an animation sequence
    const queueAnimationSequence = (sequence: AnimationSequence) => {
        setAnimationQueue(prev => [...prev, ...sequence.events]);
    };

    // Get animation state for a specific card
    const getCardAnimationState = (card: Card, playerId?: string) => {
        const cardKey = getCardKey(card, playerId);
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
            const cardKey = `${card.suit}-${card.value}`;
            const cardEventString = JSON.stringify({
                type: animationType,
                card: card, // Track individual card
                from_location: fromLocation,
                to_location: toLocation,
                player_id: playerId
            });
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
            console.log(`📍 Tracking optimistic card ${cardKey} at ${toLocation}${targetCard ? ` (covering ${targetCard.suit}${targetCard.value})` : ''}`);
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
        
        // 1. Validate first - don't do anything if validation fails
        validateAttack(game, cards);
        
        // 2. Trigger optimistic animation - single animation with all cards going to their spots
        triggerOptimisticAnimation('attack_pass', cards, 'hand', 'table', game.self?.player_id);
        
        // 3. Call server method (which will do optimistic game state updates)
        try {
        const result = await serverMethods.attack(cards);
        return result;
        } catch (error) {
            // Server rejected the attack - but check if we already reverted due to conflict detection
            console.log('\n🔴 ===== SERVER 400 ERROR - Attack rejected =====');
            console.log(`  Cards that were optimistically played:`, cards.map(c => `${c.suit}${c.value}`));
            
            let alreadyReverted = 0;
            let newReverts = 0;
            
            cards.forEach(card => {
                const cardKey = `${card.suit}-${card.value}`;
                
                // Check if this card is already being reverted OR tracking was cleared
                const isCurrentlyReverting = revertingCards.current.has(cardKey);
                const wasAlreadyReverted = !optimisticCardPositions.current.has(cardKey);
                
                // Also check if optimistic animation was cleared (conflict detection clears it)
                const attackCardEventString = JSON.stringify({
                    type: 'attack_pass',
                    card: card,
                    from_location: 'hand',
                    to_location: 'table',
                    player_id: game.self?.player_id
                });
                const optimisticAnimationCleared = !optimisticAnimations.current.has(attackCardEventString);
                
                if (isCurrentlyReverting || wasAlreadyReverted || optimisticAnimationCleared) {
                    console.log(`  ✓ ${cardKey} - already handled by conflict detection (skipping)`, {
                        reverting: isCurrentlyReverting,
                        cleared_position: wasAlreadyReverted,
                        cleared_tracking: optimisticAnimationCleared
                    });
                    alreadyReverted++;
                    return;
                }
                
                console.log(`  Creating fallback revert for ${cardKey}`);
                revertingCards.current.add(cardKey);
                
                // Get where this card currently is visually
                const visualPosition = optimisticCardPositions.current.get(cardKey);
                const fromLocation = visualPosition?.location || 'table';
                
                console.log(`    Visual position: ${fromLocation} → hand (${visualPosition ? 'tracked' : 'defaulted'})`);
                
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
                newReverts++;
                
                // Clear from optimistic tracking
                const fallbackCardEventString = JSON.stringify({
                    type: 'attack_pass',
                    card: card,
                    from_location: 'hand',
                    to_location: 'table',
                    player_id: game.self?.player_id
                });
                optimisticAnimations.current.delete(fallbackCardEventString);
            });
            
            console.log(`  📊 Summary: ${alreadyReverted} already reverted, ${newReverts} new reverts`);
            console.log(`🔴 ===== END 400 ERROR HANDLING =====\n`);
            throw error;
        }
    };

    const pass = async (cards: Card[]): Promise<{ game_id: string }> => {
        if (!game_id || !games[game_id]) {
            throw new Error('No active game');
        }
        
        const game = games[game_id];
        
        // 1. Validate first - don't do anything if validation fails
        validatePass(game, cards);
        
        // 2. Trigger optimistic animation - single animation with all cards going to their spots
        triggerOptimisticAnimation('attack_pass', cards, 'hand', 'table', game.self?.player_id);
        
        // 3. Track optimistic pass state (defender will change to next player)
        // Pass moves defender to the next player in sequence
        // first_attacker does NOT change during a pass (only changes on new round)
        const nextDefenderIndex = (game.defender + 1) % game.players.length;
        console.log(`\n🎯 ===== SETTING OPTIMISTIC PASS STATE =====`);
        console.log(`📍 Current game state: defender=${game.defender}, first_attacker=${game.first_attacker}`);
        console.log(`📍 NEW optimistic pass state: defender ${game.defender} → ${nextDefenderIndex}, first_attacker stays ${game.first_attacker}`);
        optimisticPassState.current = {
            defender: nextDefenderIndex,
            first_attacker: game.first_attacker  // Unchanged
        };
        console.log(`✅ Optimistic pass state SET`);
        console.log(`===== END SETTING OPTIMISTIC PASS =====\n`);
        
        // 4. Call server method (which will do optimistic game state updates)
        try {
        const result = await serverMethods.pass(cards);
        return result;
        } catch (error) {
            // Server rejected the pass - clear optimistic pass state and create revert animation (if not already handled)
            console.log('\n🔴 ===== PASS REJECTED - CLEARING OPTIMISTIC STATE =====');
            console.log('🔴 SERVER 400 ERROR - Pass rejected');
            console.log(`🔴 Clearing optimistic pass state: was defender=${optimisticPassState.current?.defender}, first_attacker=${optimisticPassState.current?.first_attacker}`);
            optimisticPassState.current = null;
            console.log('✅ Optimistic pass state CLEARED');
            
            // Check if conflict detection already handled these cards
            const cardsNeedingRevert = cards.filter(card => {
                const cardEventString = JSON.stringify({
                    type: 'attack_pass',
                    card: card,
                    from_location: 'hand',
                    to_location: 'table',
                    player_id: game.self?.player_id
                });
                return optimisticAnimations.current.has(cardEventString);
            });
            
            if (cardsNeedingRevert.length === 0) {
                console.log('✅ All pass cards already reverted by conflict detection - skipping fallback revert');
                console.log('===== END PASS REJECTION =====\n');
            } else {
                console.log(`⚠️ Creating fallback revert for ${cardsNeedingRevert.length} card(s) not handled by conflict detection`);
            
            cardsNeedingRevert.forEach(card => {
                const cardKey = `${card.suit}-${card.value}`;
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
                
                optimisticAnimations.current.delete(JSON.stringify({
                    type: 'attack_pass',
                    card,
                    from_location: 'hand',
                    to_location: 'table',
                    player_id: game.self?.player_id
                }));
            });
                console.log('===== END PASS REJECTION =====\n');
            }
            throw error;
        }
    };

    const pickup = async (): Promise<{ game_id: string }> => {
        if (!game_id || !games[game_id]) {
            throw new Error('No active game');
        }
        
        const game = games[game_id];
        
        // 1. Validate first - don't do anything if validation fails
        validatePickup(game);
        
        const allTableCards = game.table_battles.flatMap(battle => 
            battle.defense ? [battle.attack, battle.defense] : [battle.attack]
        );
        
        // 2. Trigger optimistic animation
        triggerOptimisticAnimation('pickup', allTableCards, 'table', 'hand', game.self?.player_id);
        
        // 3. Call server method (which will do optimistic game state updates)
        try {
        const result = await serverMethods.pickup();
        return result;
        } catch (error) {
            // Server rejected the pickup
            // Check if conflict detection already handled reverts
            const stillTracking = allTableCards.filter(card => {
                const cardEventString = JSON.stringify({
                    type: 'pickup',
                    card,
                    from_location: 'table',
                    to_location: 'hand',
                    player_id: game.self?.player_id
                });
                return optimisticAnimations.current.has(cardEventString);
            });
            
            if (stillTracking.length === 0) {
                console.log('🔴 SERVER 400 ERROR - Pickup rejected, but conflict detection already handled reverts');
                throw error;
            }
            
            console.log('🔴 SERVER 400 ERROR - Pickup rejected, creating fallback reverts');
            allTableCards.forEach(card => {
                const cardKey = `${card.suit}-${card.value}`;
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
                
                optimisticAnimations.current.delete(JSON.stringify({
                    type: 'pickup',
                    card,
                    from_location: 'table',
                    to_location: 'hand',
                    player_id: game.self?.player_id
                }));
            });
            throw error;
        }
    };

    const cover = async (coverCards: Card[], attackCards: Card[]): Promise<{ game_id: string }> => {
        if (!game_id || !games[game_id]) {
            throw new Error('No active game');
        }
        
        const game = games[game_id];
        
        // 1. Validate first - don't do anything if validation fails
        validateCover(game, coverCards, attackCards);
        
        // 2. Trigger optimistic cover animation - SINGLE animation with all cards going to their targets
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
            const cardKey = `${coverCard.suit}-${coverCard.value}`;
            
            // Find battle index for this attack
            const battleIndex = game.table_battles.findIndex(b => 
                b.attack.suit === attackCard.suit && b.attack.value === attackCard.value
            );
            
            // Track for conflict detection
            const cardEventString = JSON.stringify({
                type: 'cover',
                card: coverCard,
                from_location: 'hand',
                to_location: 'table',
                player_id: game.self?.player_id
            });
            optimisticAnimations.current.set(cardEventString, timestamp);
            
            // Track visual position with target card info for animations
            const positionInfo: any = { 
                location: 'table', 
                playerId: game.self?.player_id || '',
                target_card: attackCard,
                battle_index: battleIndex
            };
            optimisticCardPositions.current.set(cardKey, positionInfo);
            console.log(`📍 Tracking optimistic card ${cardKey} at table (covering ${attackCard.suit}${attackCard.value})`);
        });
        
        // Queue the single animation with all cover cards
        queueAnimation(animationEvent);
        
        // 3. Call server method (which will do optimistic game state updates)
        try {
        const result = await serverMethods.cover(coverCards, attackCards);
        return result;
        } catch (error) {
            // Server rejected the cover - create revert animation
            console.log('🔴 SERVER 400 ERROR - Cover rejected, creating reverts');
            coverCards.forEach(card => {
                const cardKey = `${card.suit}-${card.value}`;
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
                
                optimisticAnimations.current.delete(JSON.stringify({
                    type: 'cover',
                    card,
                    from_location: 'hand',
                    to_location: 'table',
                    player_id: game.self?.player_id
                }));
            });
            throw error;
        }
    };

    const good = async (): Promise<{ game_id: string }> => {
        // Good doesn't have an optimistic animation - it's just a signal
        const result = await serverMethods.good();
        return result;
    };

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

    return (
        <AnimationContext.Provider value={{
            isAnimating,
            currentAnimation,
            animationQueue,
            queueAnimation,
            queueAnimationSequence,
            getCardAnimationState,
            attack,
            pass,
            pickup,
            cover,
            good
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