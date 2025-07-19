import React, { useEffect, useState, useRef } from 'react';
import { Card } from '../../common/types';
import { useAnimation } from '../../contexts/AnimationContext';
import { CardFace } from './CardFace';

interface AnimatedCard {
    id: string;
    card: Card;
    startPosition: { x: number; y: number };
    endPosition: { x: number; y: number };
    progress: number;
    animationType: string;
    playerId?: string;
}

export const AnimationOverlay = () => {
    const [animatedCards, setAnimatedCards] = useState<AnimatedCard[]>([]);
    const { currentAnimation, isAnimating } = useAnimation();
    const overlayRef = useRef<HTMLDivElement>(null);

    // Helper function to get element position relative to viewport
    const getElementPosition = (element: HTMLElement): { x: number; y: number } => {
        const rect = element.getBoundingClientRect();
        return {
            x: rect.left + rect.width / 2,
            y: rect.top + rect.height / 2
        };
    };

    // Helper function to find element by data attributes with retry logic
    const findElementByLocation = (location: string, playerId?: string, cardSuit?: number, cardValue?: number, retryCount = 0): HTMLElement | null => {
        // For table cards, try to find the specific card element first
        if (location === 'table' && cardSuit !== undefined && cardValue !== undefined) {
            const cardSelector = `[data-card="${cardSuit}-${cardValue}"]`;
            const cardElement = document.querySelector(cardSelector) as HTMLElement | null;
            if (cardElement) {
                return cardElement;
            }
        }
        
        // First try to find the specific card (for current user's hand)
        if (playerId && cardSuit !== undefined && cardValue !== undefined) {
            const specificCardSelector = `[data-location="${location}"][data-player-id="${playerId}"][data-card="${cardSuit}-${cardValue}"]`;
            const specificElement = document.querySelector(specificCardSelector) as HTMLElement | null;
            if (specificElement) {
                return specificElement;
            }
        }
        
        // Fall back to general location area
        let generalSelector = `[data-location="${location}"]`;
        if (playerId) {
            generalSelector += `[data-player-id="${playerId}"]`;
        }
        
        const element = document.querySelector(generalSelector) as HTMLElement | null;
        
        // If element not found and we haven't retried too many times, try again after a short delay
        if (!element && retryCount < 3) {
            setTimeout(() => {
                return findElementByLocation(location, playerId, cardSuit, cardValue, retryCount + 1);
            }, 50);
            return null;
        }
        
        return element;
    };

    // Helper function to get fallback positions when elements aren't found
    const getFallbackPosition = (location: string, playerId?: string): { x: number; y: number } => {
        const centerX = window.innerWidth / 2;
        const centerY = window.innerHeight / 2;
        
        switch (location) {
            case 'hand':
                return { x: centerX, y: window.innerHeight - 100 };
            case 'table':
                return { x: centerX, y: centerY };
            case 'deck':
                return { x: 100, y: 120 };
            case 'discard':
                return { x: window.innerWidth - 100, y: 120 }; // Top-right corner
            default:
                return { x: centerX, y: centerY };
        }
    };

    useEffect(() => {
        if (!currentAnimation || !isAnimating) {
            return;
        }

        const { type, cards, from_location, to_location, player_id } = currentAnimation;

        // Handle magic_transition separately since it doesn't have cards
        if (type === 'magic_transition') {
            return; // Magic transitions are just messages, no visual animation needed
        }
        
        // All other animation types need cards
        if (!cards || cards.length === 0) {
            return;
        }

        // Small delay to ensure DOM is ready
        setTimeout(() => {
            // Calculate positions for each card
            const newAnimatedCards: AnimatedCard[] = [];

            cards.forEach((card, index) => {
                // Find source element
                let sourceElement: HTMLElement | null = null;
                let startPos: { x: number; y: number };
                
                if (from_location === 'hand') {
                    sourceElement = findElementByLocation('hand', player_id, card.suit, card.value);
                } else if (from_location === 'deck') {
                    sourceElement = findElementByLocation('deck');
                } else if (from_location === 'table') {
                    sourceElement = findElementByLocation('table', undefined, card.suit, card.value);
                }

                if (sourceElement) {
                    startPos = getElementPosition(sourceElement);
                } else {
                    startPos = getFallbackPosition(from_location || 'hand', player_id);
                }

                // Find destination element
                let destinationElement: HTMLElement | null = null;
                let endPos: { x: number; y: number };
                
                if (to_location === 'hand') {
                    destinationElement = findElementByLocation('hand', player_id);
                } else if (to_location === 'table') {
                    destinationElement = findElementByLocation('table');
                } else if (to_location === 'discard') {
                    destinationElement = findElementByLocation('discard');
                }

                if (destinationElement) {
                    endPos = getElementPosition(destinationElement);
                } else {
                    endPos = getFallbackPosition(to_location || 'table', player_id);
                }

                // Add some offset for multiple cards
                const offset = index * 5;
                endPos.x += offset;
                endPos.y += offset;

                newAnimatedCards.push({
                    id: `${card.suit}-${card.value}-${player_id}-${Date.now()}-${index}`,
                    card,
                    startPosition: startPos,
                    endPosition: endPos,
                    progress: 0,
                    animationType: type,
                    playerId: player_id
                });
            });

            setAnimatedCards(newAnimatedCards);

            // Animate the cards - Make it very slow to see what's happening
            const animationDuration = 2000; // Very slow for debugging
            const startTime = Date.now();

            const animate = () => {
                const currentTime = Date.now();
                const elapsed = currentTime - startTime;
                const progress = Math.min(elapsed / animationDuration, 1);

                setAnimatedCards(prev => 
                    prev.map(animatedCard => ({
                        ...animatedCard,
                        progress
                    }))
                );

                if (progress < 1) {
                    requestAnimationFrame(animate);
                } else {
                    // Animation complete, clear animated cards
                    setTimeout(() => {
                        setAnimatedCards([]);
                    }, 50);
                }
            };

            requestAnimationFrame(animate);
        }, 50); // Small delay to ensure DOM is ready

    }, [currentAnimation, isAnimating]);

    if (animatedCards.length === 0) {
        return null;
    }

    return (
        <div 
            ref={overlayRef}
            style={{
                position: 'fixed',
                top: 0,
                left: 0,
                width: '100%',
                height: '100%',
                pointerEvents: 'none',
                zIndex: 10000,
                // Add a semi-transparent background to see the overlay
                backgroundColor: 'rgba(255, 0, 0, 0.1)'
            }}
        >
            {animatedCards.map(animatedCard => {
                const { startPosition, endPosition, progress, card, id } = animatedCard;
                
                // Calculate current position using easing
                const easeOutCubic = (t: number) => 1 - Math.pow(1 - t, 3);
                const easedProgress = easeOutCubic(progress);
                
                const currentX = startPosition.x + (endPosition.x - startPosition.x) * easedProgress;
                const currentY = startPosition.y + (endPosition.y - startPosition.y) * easedProgress;

                return (
                    <div
                        key={id}
                        style={{
                            position: 'absolute',
                            left: currentX - 35, // Half card width (bigger card)
                            top: currentY - 45,  // Half card height (bigger card)
                            transform: `scale(${1.5 + progress * 0.3})`, // Much bigger
                            opacity: 1,
                            transition: 'none',
                            border: '3px solid red', // Bright red border
                            borderRadius: '8px',
                            boxShadow: '0 0 15px rgba(255, 0, 0, 0.8)' // Glowing effect
                        }}
                    >
                        <CardFace 
                            card={card}
                            isAnimationOverlay={true}
                            style={{
                                boxShadow: `0 ${progress * 10}px ${progress * 20}px rgba(0,0,0,0.4)`,
                                filter: `brightness(${1 + progress * 0.2})`
                            }}
                        />
                    </div>
                );
            })}
        </div>
    );
}; 