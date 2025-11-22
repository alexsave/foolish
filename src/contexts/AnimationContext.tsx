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
    battle_index?: number;
    message?: string;
    game_state?: Game; // intermediate game state after this event
}

interface AnimationSequence {
    type: 'animation_sequence';
    events: AnimationEvent[];
    sequence_id: string;
    timestamp: number;
}

interface AnimationContextType {
    isAnimating: boolean;
    currentAnimation: AnimationEvent | null;
    animationQueue: AnimationEvent[];
    queueAnimation: (event: AnimationEvent) => void;
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
    const [currentAnimation, setCurrentAnimation] = useState<AnimationEvent | null>(null);
    const [animationQueue, setAnimationQueue] = useState<AnimationEvent[]>([]);
    
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
    const animationQueueRef = useRef<AnimationEvent[]>([]);
    const isAnimatingRef = useRef<boolean>(false);
    const pendingCompletionCallbackRef = useRef<(() => void) | null>(null);
    const remainingSequenceEventsRef = useRef<number>(0);

    // Bot bump timer ref
    const botBumpTimerRef = useRef<NodeJS.Timeout | null>(null);

    // Keep track of processed sequence IDs and event content to avoid duplicates
    const processedSequenceIds = useRef<Set<string>>(new Set());
    const processedEventContent = useRef<Set<string>>(new Set());
    
    // Store the current game ID for this animation sequence
    const currentGameIdRef = useRef<string | null>(null);

    // Track optimistically triggered animations to avoid server duplicates
    // Map of animation hash -> timestamp when it was added
    const optimisticAnimations = useRef<Map<string, number>>(new Map());
    
    // Store channel reference for proper cleanup
    const gameUserChannelRef = useRef<any>(null);
    
    // Simple retry interval for animation channel
    const animationChannelRetryInterval = useRef(1000);

    // Bot bump timer management
    const startBotBumpTimer = useCallback(() => {
        // Clear existing timer
        if (botBumpTimerRef.current) {
            clearTimeout(botBumpTimerRef.current);
        }
        
        // Check if there are any AI players in the game
        const currentGame = url_game_id ? games[url_game_id] : null;
        const hasAIPlayers = currentGame?.players?.some(player => player.is_ai) || false;
        
        // Only start timer if there are AI players
        if (!hasAIPlayers) {
            return;
        }
        
        // Start new 12-second timer
        botBumpTimerRef.current = setTimeout(() => {
            if (url_game_id) {
                supabase.functions.invoke('bot_bump', { 
                    body: { game_id: url_game_id } 
                }).catch(error => {
                    console.error('Bot bump failed:', error);
                });
                startBotBumpTimer();
            }
        }, BOT_BUMP_TIMEOUT);
    }, [url_game_id]);

    // Start bot bump timer when component mounts and game is loaded
    useEffect(() => {
        if (url_game_id) {
            startBotBumpTimer();
        }
        
        // Cleanup on unmount
        return () => {
            if (botBumpTimerRef.current) {
                clearTimeout(botBumpTimerRef.current);
            }
        };
    }, [url_game_id, startBotBumpTimer]);

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
                            animationChannelRetryInterval.current = 1000; // Reset retry interval on success
                        } else if (status === 'CHANNEL_ERROR' || status === 'TIMED_OUT' || status === 'CLOSED') {
                            setTimeout(() => {
                                subscribeToGameAnimations().catch(console.error);
                                animationChannelRetryInterval.current *= 2; // Double the interval
                            }, animationChannelRetryInterval.current);
                        } else {
                        }
                    });
            } catch (error) {
                console.error('Error setting up game animation subscription:', error);
                setTimeout(() => {
                    subscribeToGameAnimations().catch(console.error);
                    animationChannelRetryInterval.current *= 2; // Double the interval
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
            const nonOptimisticEvents: AnimationEvent[] = [];
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
                        const cardEventString = JSON.stringify({
                            type: serverEvent.type,
                            card: card,
                            from_location: serverEvent.from_location,
                            to_location: serverEvent.to_location,
                            player_id: serverEvent.player_id
                        });
                        const timestamp = optimisticAnimations.current.get(cardEventString);
                        const wasDeleted = optimisticAnimations.current.delete(cardEventString);
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
            message.events.forEach((animEvent: AnimationEvent, index: number) => {
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
            
            // Store the completion callback to update final game state
            pendingCompletionCallbackRef.current = () => {
                if (message.game) {
                    updateGameState(message.game.id, message.game);
                }
            };
            remainingSequenceEventsRef.current = message.events.length;
            
            // Queue all events from the sequence
            setAnimationQueue(prev => [...prev, ...message.events]);
        }
    };

    // Helper function to create a unique card key
    const getCardKey = (card: Card, playerId?: string) => {
        return `${card.suit}-${card.value}-${playerId || 'global'}`;
    };

    // Process the animation queue
    const processAnimationQueue = useCallback(() => {
        if (animationQueueRef.current.length === 0) {
            setIsAnimating(false);
            setCurrentAnimation(null);
            
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
        
        // Reset bot bump timer when processing animations
        startBotBumpTimer();
        
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
                updateGameState(currentGameIdRef.current, nextAnimation.game_state);
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
            }

            // Decrement remaining sequence events count if we're tracking a sequence
            if (pendingCompletionCallbackRef.current && remainingSequenceEventsRef.current > 0) {
                remainingSequenceEventsRef.current--;
            }
            
            // Process next animation after a short delay
            setTimeout(processAnimationQueue, 100);
        }, ANIMATION_TIME);
    }, [updateGameState, startBotBumpTimer]);

    // Start processing queue when items are added and no animation is running
    useEffect(() => {
        if (animationQueue.length > 0 && !isAnimating) {
            processAnimationQueue();
        }
    }, [animationQueue, isAnimating, processAnimationQueue]);

    // Queue a single animation
    const queueAnimation = (event: AnimationEvent) => {
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
        const animationEvent: AnimationEvent = {
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
            const cardEventString = JSON.stringify({
                type: animationType,
                card: card, // Track individual card
                from_location: fromLocation,
                to_location: toLocation,
                player_id: playerId
            });
            optimisticAnimations.current.set(cardEventString, timestamp);
        });
        
        // Reset bot bump timer when triggering optimistic animations
        startBotBumpTimer();
        
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
        const result = await serverMethods.attack(cards);
        
        return result;
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
        
        // 3. Call server method (which will do optimistic game state updates)
        const result = await serverMethods.pass(cards);
        
        return result;
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
        const result = await serverMethods.pickup();
        
        return result;
    };

    const cover = async (coverCards: Card[], attackCards: Card[]): Promise<{ game_id: string }> => {
        if (!game_id || !games[game_id]) {
            throw new Error('No active game');
        }
        
        const game = games[game_id];
        
        // 1. Validate first - don't do anything if validation fails
        validateCover(game, coverCards, attackCards);
        
        // 2. Trigger optimistic cover animation - single animation with all cards going to their targets
        triggerOptimisticAnimation('cover', coverCards, 'hand', 'table', game.self?.player_id);
        
        // 3. Call server method (which will do optimistic game state updates)
        const result = await serverMethods.cover(coverCards, attackCards);
        
        return result;
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
                clearTimeout(botBumpTimerRef.current);
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