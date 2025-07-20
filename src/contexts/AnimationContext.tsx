import React, { createContext, useContext, useEffect, useState, useRef, useCallback } from 'react';
import { Card, PersonalGame, Game, PublicPlayer } from '../common/types';
import { useServer } from './ServerContext';
import { useAuth } from './AuthContext';
import { useParams } from 'react-router-dom';
import supabase from '../backend/Connector';
import { ANIMATION_TIME } from '../constants/constants';
import { validateAttack, validatePass, validatePickup, validateCover } from '../utils/gameValidation';
import { get_next_player_index, card_comp } from '../common/common_utils';

// Animation timing constant
export { ANIMATION_TIME } from '../constants/constants';

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

    // Keep track of processed sequence IDs and event content to avoid duplicates
    const processedSequenceIds = useRef<Set<string>>(new Set());
    const processedEventContent = useRef<Set<string>>(new Set());
    
    // Store the current game ID for this animation sequence
    const currentGameIdRef = useRef<string | null>(null);

    // Track optimistically triggered animations to avoid server duplicates
    const optimisticAnimations = useRef<Set<string>>(new Set());
    
    // Store channel reference for proper cleanup
    const gameUserChannelRef = useRef<any>(null);
    
    // Simple retry interval for animation channel
    const animationChannelRetryInterval = useRef(1000);

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
                            console.log('[WEBSOCKET] Successfully subscribed to channel:', `gu-${url_game_id}-${user_id}`);
                            animationChannelRetryInterval.current = 1000; // Reset retry interval on success
                        } else if (status === 'CHANNEL_ERROR' || status === 'TIMED_OUT' || status === 'CLOSED') {
                            console.error('[WEBSOCKET] Animation channel error:', err || 'Unknown error');
                            console.log(`Retrying animation channel connection in ${animationChannelRetryInterval.current}ms`);
                            setTimeout(() => {
                                subscribeToGameAnimations().catch(console.error);
                                animationChannelRetryInterval.current *= 2; // Double the interval
                            }, animationChannelRetryInterval.current);
                        } else {
                            console.log('[WEBSOCKET] Channel status:', status, err);
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
        console.log('[ANIMATION_EVENTS] Animation message received:', message);
        
        if (message.events && Array.isArray(message.events)) {
            // Store the game ID for use during animations
            if (message.game?.id) {
                currentGameIdRef.current = message.game.id;
            }
            
            // Check for duplicate sequence_id FIRST (before checking optimistic events)
            const sequenceId = message.sequence_id || message.gameId + '-' + Date.now();
            console.log(`[DUPLICATION] Checking sequence_id for duplicates:`, sequenceId);
            console.log(`[DUPLICATION] Currently processed sequence_ids:`, Array.from(processedSequenceIds.current));
            
            if (processedSequenceIds.current.has(sequenceId)) {
                console.log(`[DUPLICATION] Duplicate sequence_id detected, ignoring:`, sequenceId);
                return;
            }
            
            // Also check event content as backup
            const eventsString = JSON.stringify(message.events);
            console.log(`[DUPLICATION] Checking event content for duplicates (first 100 chars):`, eventsString.substring(0, 100) + '...');
            
            if (processedEventContent.current.has(eventsString)) {
                console.log(`[DUPLICATION] Duplicate event content detected, ignoring`);
                return;
            }
            
            // Mark as processed early to prevent race conditions
            processedSequenceIds.current.add(sequenceId);
            processedEventContent.current.add(eventsString);
            console.log(`[DUPLICATION] Marked sequence_id as processed:`, sequenceId);
            
            // Check if any of these events were triggered optimistically
            const serverEvents = message.events;
            console.log(`[DUPLICATION] Checking ${serverEvents.length} server events for duplicates`);
            console.log(`[DUPLICATION] Current optimistic set size:`, optimisticAnimations.current.size);
            console.log(`[DUPLICATION] Current optimistic set contents:`, Array.from(optimisticAnimations.current));
            
            const hasOptimisticEvent = serverEvents.some((serverEvent: any, index: number) => {
                const serverEventString = JSON.stringify({
                    type: serverEvent.type,
                    cards: serverEvent.cards,
                    from_location: serverEvent.from_location,
                    to_location: serverEvent.to_location,
                    player_id: serverEvent.player_id
                });
                console.log(`[DUPLICATION] Server event ${index + 1}:`, {
                    type: serverEvent.type,
                    cards: serverEvent.cards,
                    from_location: serverEvent.from_location,
                    to_location: serverEvent.to_location,
                    player_id: serverEvent.player_id
                });
                console.log(`[DUPLICATION] Server event ${index + 1} string:`, serverEventString);
                const isOptimistic = optimisticAnimations.current.has(serverEventString);
                console.log(`[DUPLICATION] Server event ${index + 1} is optimistic:`, isOptimistic);
                return isOptimistic;
            });

            console.log(`[DUPLICATION] Has optimistic event:`, hasOptimisticEvent);

            if (hasOptimisticEvent) {
                console.log('[OPTIMISTIC] Ignoring server animations - already triggered optimistically');
                // Clear the optimistic animations since server confirmed them
                serverEvents.forEach((serverEvent: any, index: number) => {
                    const serverEventString = JSON.stringify({
                        type: serverEvent.type,
                        cards: serverEvent.cards,
                        from_location: serverEvent.from_location,
                        to_location: serverEvent.to_location,
                        player_id: serverEvent.player_id
                    });
                    console.log(`[DUPLICATION] Removing server event ${index + 1} from optimistic set:`, serverEventString);
                    const wasDeleted = optimisticAnimations.current.delete(serverEventString);
                    console.log(`[DUPLICATION] Server event ${index + 1} was deleted:`, wasDeleted);
                });
                
                console.log(`[DUPLICATION] After cleanup, optimistic set size:`, optimisticAnimations.current.size);
                console.log(`[DUPLICATION] After cleanup, optimistic set contents:`, Array.from(optimisticAnimations.current));
                
                // Still update game state from server, but skip animations
                if (message.game) {
                    updateGameState(message.game.id, message.game);
                }
                return;
            } else {
                console.log('[DUPLICATION] No optimistic events found, processing server animations normally');
            }
            
            // Log which events have intermediate game states
            console.log('[ANIMATION] Events breakdown:');
            message.events.forEach((animEvent: AnimationEvent, index: number) => {
                const hasGameState = !!animEvent.game_state;
                const stateInfo = hasGameState ? 
                    `table:${animEvent.game_state!.table_battles?.length || 0}, hands:${animEvent.game_state!.players?.map(p => p.hand?.length || 0).join(',')}` :
                    'NO STATE';
                console.log(`  ${index + 1}. ${animEvent.type} (${animEvent.message}) - ${stateInfo}`);
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
                console.log('[ANIMATION] Animation sequence completed, updating final game state');
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
            console.log('[QUEUE] Animation queue is empty, stopping processing');
            setIsAnimating(false);
            setCurrentAnimation(null);
            
            // Clear processed event content when queue is empty (allows future legitimate duplicates)
            if (processedEventContent.current.size > 0) {
                processedEventContent.current.clear();
            }
            
            // Check if we have a pending completion callback and we've finished the sequence
            if (pendingCompletionCallbackRef.current && remainingSequenceEventsRef.current === 0) {
                console.log('[ANIMATION] Animation sequence completed, calling completion callback');
                const callback = pendingCompletionCallbackRef.current;
                pendingCompletionCallbackRef.current = null;
                remainingSequenceEventsRef.current = 0;
                callback();
            }
            
            return;
        }

        const nextAnimation = animationQueueRef.current[0];
        console.log('[QUEUE] Processing animation from queue:', {
            type: nextAnimation.type,
            player_id: nextAnimation.player_id,
            cards: nextAnimation.cards,
            from_location: nextAnimation.from_location,
            to_location: nextAnimation.to_location,
            message: nextAnimation.message
        });
        console.log('[QUEUE] Remaining animations in queue after this one:', animationQueueRef.current.length - 1);
        
        setCurrentAnimation(nextAnimation);
        setAnimationQueue(prev => prev.slice(1));
        setIsAnimating(true);

        // Log intermediate game state info
        if (nextAnimation.game_state) {
            console.log(`[ANIMATION] Processing ${nextAnimation.type} with intermediate game state:`);
            console.log(`  - Table battles: ${nextAnimation.game_state.table_battles?.length || 0} battles`);
            console.log(`  - Deck size: ${nextAnimation.game_state.deck?.length || 0}`);
            console.log(`  - Discard pile: ${nextAnimation.game_state.discard_pile_length || 0}`);
            console.log(`  - Current defender: ${nextAnimation.game_state.players?.[nextAnimation.game_state.defender]?.name || 'unknown'}`);
            console.log(`  - Player hands:`, nextAnimation.game_state.players?.map(p => `${p.name}: ${p.hand?.length || 0} cards`));
        } else {
            console.log(`[ANIMATION] Processing ${nextAnimation.type} WITHOUT intermediate game state`);
        }

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
                console.log('[ANIMATION] Updating game state after animation completes');
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
                console.log('[ANIMATION] Animation completed, remaining sequence events:', remainingSequenceEventsRef.current);
            }
            
            // Process next animation after a short delay
            setTimeout(processAnimationQueue, 100);
        }, ANIMATION_TIME);
    }, [updateGameState]);

    // Start processing queue when items are added and no animation is running
    useEffect(() => {
        if (animationQueue.length > 0 && !isAnimating) {
            console.log('[ANIMATION] Starting animation queue processing, queue length:', animationQueue.length);
            processAnimationQueue();
        }
    }, [animationQueue, isAnimating, processAnimationQueue]);

    // Queue a single animation
    const queueAnimation = (event: AnimationEvent) => {
        console.log('[QUEUE] Queueing animation:', {
            type: event.type,
            player_id: event.player_id,
            cards: event.cards,
            from_location: event.from_location,
            to_location: event.to_location,
            message: event.message
        });
        setAnimationQueue(prev => {
            console.log('[QUEUE] Animation queue length before adding:', prev.length);
            const newQueue = [...prev, event];
            console.log('[QUEUE] Animation queue length after adding:', newQueue.length);
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
    const triggerOptimisticAnimation = (animationType: string, cards: Card[], fromLocation: string, toLocation: string, playerId?: string) => {
        const animationEvent: AnimationEvent = {
            type: animationType as any,
            cards: cards,
            from_location: fromLocation as any,
            to_location: toLocation as any,
            player_id: playerId,
            message: `Optimistic ${animationType} animation`
        };
        
        // Track this animation to avoid duplicates from server
        const eventString = JSON.stringify({
            type: animationType,
            cards: cards,
            from_location: fromLocation,
            to_location: toLocation,
            player_id: playerId
        });
        optimisticAnimations.current.add(eventString);
        
        // Queue the optimistic animation immediately
        queueAnimation(animationEvent);
        
        console.log(`[OPTIMISTIC] Triggered ${animationType} animation:`, animationEvent);
        console.log(`[OPTIMISTIC] Event string added to set:`, eventString);
        console.log(`[OPTIMISTIC] Current optimistic set size:`, optimisticAnimations.current.size);
        console.log(`[OPTIMISTIC] Current optimistic set contents:`, Array.from(optimisticAnimations.current));
        return eventString;
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
        
        // 2. Trigger optimistic animation
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
        
        // 2. Trigger optimistic animation
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
        
        // 2. Trigger optimistic cover animations (one for each card)
        for (let i = 0; i < coverCards.length; i++) {
            const coverCard = coverCards[i];
            triggerOptimisticAnimation('cover', [coverCard], 'hand', 'table', game.self?.player_id);
        }
        
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