import React, { createContext, useContext, useEffect, useState, useRef, useCallback } from 'react';
import { Card, PersonalGame } from '../common/types';
import { useServer } from './ServerContext';

interface AnimationEvent {
    type: 'magic_transition' | 'deal' | 'flipped' | 'defender_move' | 'attack_pass' | 'cover' | 'pickup' | 'discard' | 'out' | 'refill' | 'cards_to_trash';
    player_id?: string;
    cards?: Card[];
    from_location?: 'deck' | 'hand' | 'table' | 'discard';
    to_location?: 'deck' | 'hand' | 'table' | 'discard' | 'flipped';
    target_card?: Card;
    battle_index?: number;
    message?: string;
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
}

const AnimationContext = createContext<AnimationContextType | null>(null);

export const AnimationProvider = ({ children }: { children: React.ReactNode }) => {
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
    const progressIntervalRef = useRef<NodeJS.Timeout | null>(null);
    const animationQueueRef = useRef<AnimationEvent[]>([]);
    const isAnimatingRef = useRef<boolean>(false);
    const pendingCompletionCallbackRef = useRef<(() => void) | null>(null);
    const remainingSequenceEventsRef = useRef<number>(0);

    // Keep track of processed sequence IDs to avoid duplicates
    const processedSequenceIds = useRef<Set<string>>(new Set());

    // Listen for animation events from ServerContext
    useEffect(() => {
        const handleAnimationEvents = (event: any) => {
            console.log('[ANIMATION] Animation events received:', event.detail);
            console.log('[ANIMATION] Current queue length before adding:', animationQueueRef.current.length);
            
            if (event.detail.events && Array.isArray(event.detail.events)) {
                // Check for duplicate sequences using sequence_id if available
                const sequenceId = event.detail.sequence_id || event.detail.gameId + '-' + Date.now();
                
                if (processedSequenceIds.current.has(sequenceId)) {
                    console.log('[ANIMATION] Skipping duplicate sequence:', sequenceId);
                    return;
                }
                
                processedSequenceIds.current.add(sequenceId);
                
                // Clean up old sequence IDs (keep only last 100)
                if (processedSequenceIds.current.size > 100) {
                    const ids = Array.from(processedSequenceIds.current);
                    processedSequenceIds.current = new Set(ids.slice(-50));
                }
                
                // Store the completion callback if provided
                if (event.detail.onAnimationComplete) {
                    console.log('[ANIMATION] Setting completion callback, events count:', event.detail.events.length);
                    pendingCompletionCallbackRef.current = event.detail.onAnimationComplete;
                    remainingSequenceEventsRef.current = event.detail.events.length;
                }
                
                // Queue all events from the sequence
                console.log('[ANIMATION] Adding events to queue:', event.detail.events);
                console.log('[ANIMATION] Event types being queued:', event.detail.events.map((e: any) => e.type));
                setAnimationQueue(prev => [...prev, ...event.detail.events]);
            }
        };

        window.addEventListener('gameAnimationEvents', handleAnimationEvents);
        
        return () => {
            window.removeEventListener('gameAnimationEvents', handleAnimationEvents);
        };
    }, []);

    // Helper function to create a unique card key
    const getCardKey = (card: Card, playerId?: string) => {
        return `${card.suit}-${card.value}-${playerId || 'global'}`;
    };

    // Process the animation queue
    const processAnimationQueue = useCallback(() => {
        if (animationQueueRef.current.length === 0) {
            setIsAnimating(false);
            setCurrentAnimation(null);
            
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
        console.log('[ANIMATION] Full queue before processing:', animationQueueRef.current.map((e: any) => e.type));
        console.log('[ANIMATION] About to dequeue and process event:', nextAnimation.type);
        setCurrentAnimation(nextAnimation);
        setAnimationQueue(prev => prev.slice(1));
        setIsAnimating(true);

        console.log('[ANIMATION] Processing animation:', nextAnimation);
        console.log('[ANIMATION] Animation type:', nextAnimation.type, 'Has cards:', !!nextAnimation.cards, 'Cards count:', nextAnimation.cards?.length || 0);
        
        // Special debugging for cards_to_trash
        if (nextAnimation.type === 'cards_to_trash') {
            console.log('[ANIMATION] CARDS_TO_TRASH DEBUG:');
            console.log('[ANIMATION] Full event:', JSON.stringify(nextAnimation, null, 2));
            console.log('[ANIMATION] Cards property:', nextAnimation.cards);
            console.log('[ANIMATION] From location:', nextAnimation.from_location);
            console.log('[ANIMATION] To location:', nextAnimation.to_location);
            console.log('[ANIMATION] Setting currentAnimation to cards_to_trash event');
        }

        // Start tracking cards in this animation
        if (nextAnimation.cards && nextAnimation.cards.length > 0) {
            const currentTime = Date.now();
            setAnimatingCards(prev => {
                const newAnimatingCards = new Map(prev);
                
                nextAnimation.cards!.forEach(card => {
                    const cardKey = getCardKey(card, nextAnimation.player_id);
                    newAnimatingCards.set(cardKey, {
                        animationType: nextAnimation.type,
                        progress: 0,
                        fromLocation: nextAnimation.from_location || null,
                        toLocation: nextAnimation.to_location || null,
                        startTime: currentTime
                    });
                });
                
                return newAnimatingCards;
            });

            // Update progress over the animation duration
            const updateProgress = () => {
                const now = Date.now();
                setAnimatingCards(prev => {
                    const updated = new Map(prev);
                    let allCompleted = true;

                    nextAnimation.cards!.forEach(card => {
                        const cardKey = getCardKey(card, nextAnimation.player_id);
                        const cardAnimation = updated.get(cardKey);
                        
                        if (cardAnimation) {
                            const elapsed = now - cardAnimation.startTime;
                            const progress = Math.min(elapsed / 2000, 1); // 2000ms animation duration
                            
                            if (progress < 1) {
                                allCompleted = false;
                            }
                            
                            updated.set(cardKey, {
                                ...cardAnimation,
                                progress
                            });
                        }
                    });

                    if (!allCompleted) {
                        progressIntervalRef.current = setTimeout(updateProgress, 16); // ~60fps
                    }
                    
                    return updated;
                });
            };

            updateProgress();
        }

        // Animation duration: 2000ms to match AnimationOverlay
        timeoutRef.current = setTimeout(() => {
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

            // Clear the progress interval
            if (progressIntervalRef.current) {
                clearTimeout(progressIntervalRef.current);
                progressIntervalRef.current = null;
            }

            // Decrement remaining sequence events count if we're tracking a sequence
            if (pendingCompletionCallbackRef.current && remainingSequenceEventsRef.current > 0) {
                remainingSequenceEventsRef.current--;
                console.log('[ANIMATION] Animation completed, remaining sequence events:', remainingSequenceEventsRef.current);
            }
            
            // Process next animation after 2000ms delay
            setTimeout(processAnimationQueue, 200);
        }, 2000);
    }, []);

    // Start processing queue when items are added and no animation is running
    useEffect(() => {
        if (animationQueue.length > 0 && !isAnimating) {
            console.log('[ANIMATION] Starting animation queue processing, queue length:', animationQueue.length);
            processAnimationQueue();
        }
    }, [animationQueue, isAnimating, processAnimationQueue]);

    // Queue a single animation
    const queueAnimation = (event: AnimationEvent) => {
        setAnimationQueue(prev => [...prev, event]);
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



    // Cleanup timeouts on unmount
    useEffect(() => {
        return () => {
            if (timeoutRef.current) {
                clearTimeout(timeoutRef.current);
            }
            if (progressIntervalRef.current) {
                clearTimeout(progressIntervalRef.current);
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
            getCardAnimationState
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