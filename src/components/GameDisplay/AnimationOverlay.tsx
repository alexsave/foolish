import { useEffect, useState, useRef } from 'react';
import { Card } from '@shared/types.ts';
import { ANIMATION_TIME, useAnimation } from '../../contexts/AnimationContext';
import { CardFace } from './CardFace';
import { CardBack } from './CardBack';
import { useServer } from '../../contexts/ServerContext';
import { canCover } from '@shared/common_utils.ts';

// Table-slot geometry cache (Stage 9). The on-table battle layout is a function of
// only (how many battle slots there are, the viewport size) — the 4th slot in a
// 4-slot table sits in the same place as the 4th slot in any other 4-slot table, so
// once measured we never need to re-measure that (count, index) at that viewport.
// This lets us skip the expensive measure path below (create placeholders → force a
// synchronous reflow → read rects → remove) on every attack after the first of each
// table size. The viewport is part of the KEY so a resize can't return stale
// coordinates; we also clear the whole cache on resize to bound its size and drop
// anything a layout/theme change may have shifted.
const tableSlotPositionCache = new Map<string, { x: number; y: number }>();
// A slot is identified by the table's total slot count + the slot's absolute index,
// at a given viewport.
const tableSlotKey = (totalSlots: number, slotIndex: number): string =>
    `${window.innerWidth}x${window.innerHeight}|${totalSlots}|${slotIndex}`;

interface AnimatedCard {
    id: string;
    card: Card;
    startPosition: { x: number; y: number };
    endPosition: { x: number; y: number };
    progress: number;
    animationType: string;
    playerId?: string;
    isSanitizedRefill?: boolean;
    cardCount?: number;
    isRevert?: boolean; // Flag for reverted optimistic animations
}

export const AnimationOverlay = () => {
    const [animatedCards, setAnimatedCards] = useState<AnimatedCard[]>([]);
    const { currentAnimation, isAnimating } = useAnimation();
    const { game } = useServer();
    const overlayRef = useRef<HTMLDivElement>(null);

    // Invalidate the table-slot geometry cache on resize (Stage 9). The cache key
    // already includes the viewport, so a resized window can't read stale
    // coordinates; clearing on top of that bounds the map's size and drops entries a
    // reflow/theme shift may have moved.
    useEffect(() => {
        const onResize = () => tableSlotPositionCache.clear();
        window.addEventListener('resize', onResize);
        return () => window.removeEventListener('resize', onResize);
    }, []);

    // Helper function to get element position relative to viewport
    const getElementPosition = (element: HTMLElement): { x: number; y: number } => {
        const rect = element.getBoundingClientRect();
        return {
            x: rect.left + rect.width / 2,
            y: rect.top + rect.height / 2
        };
    };

    // Helper function to find element by data attributes with retry logic
    const findElementByLocation = (location: string, playerId?: string, cardSuit?: number, cardValue?: number, battleIndex?: number, retryCount = 0): HTMLElement | null => {
        // For table destinations, try specific targeting first
        if (location === 'table') {
            // If we have a battle index, try to find the specific battle container
            if (battleIndex !== undefined) {
                const battleSelector = `[data-battle-index="${battleIndex}"]`;
                const battleElement = document.querySelector(battleSelector) as HTMLElement | null;
                if (battleElement) {
                    return battleElement;
                }
            }
            
            // If we have specific card coordinates, try to find that card
            if (cardSuit !== undefined && cardValue !== undefined) {
                const cardSelector = `[data-card="${cardSuit}-${cardValue}"]`;
                const cardElement = document.querySelector(cardSelector) as HTMLElement | null;
                if (cardElement) {
                    return cardElement;
                }
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
                return findElementByLocation(location, playerId, cardSuit, cardValue, battleIndex, retryCount + 1);
            }, 50);
            return null;
        }
        
        return element;
    };

    // Helper function to create invisible placeholders and measure their positions
    const measurePlaceholderPositions = (type: string, cards: Card[], player_id?: string): Map<string, { x: number; y: number }> => {
        const positions = new Map<string, { x: number; y: number }>();
        
        if (type === 'attack_pass') {
            const currentBattleCount = game?.table_battles?.length || 0;
            // After this attack lands the table has currentBattleCount + cards.length
            // slots; each new card targets the slot at (currentBattleCount + index).
            const totalSlots = currentBattleCount + cards.length;

            // Fast path: if every target slot's geometry is already cached for this
            // table size + viewport, return it and skip the placeholder/reflow work.
            const cached = new Map<string, { x: number; y: number }>();
            let allCached = true;
            for (let index = 0; index < cards.length; index++) {
                const hit = tableSlotPositionCache.get(tableSlotKey(totalSlots, currentBattleCount + index));
                if (!hit) { allCached = false; break; }
                cached.set(`${index}`, hit);
            }
            if (allCached) {
                return cached;
            }

            // Find the table battles container
            const tableBattlesElement = document.querySelector('[data-location="table"]')?.parentElement;
            if (!tableBattlesElement) return positions;

            // Create invisible placeholder battle containers
            const placeholders: HTMLElement[] = [];
            cards.forEach((card, index) => {
                const placeholder = document.createElement('div');
                placeholder.style.cssText = `
                    display: flex;
                    flex-direction: column;
                    align-items: center;
                    position: relative;
                    width: 60px;
                    height: 80px;
                    margin: 5px;
                    justify-content: center;
                    visibility: hidden;
                    pointer-events: none;
                `;
                placeholder.setAttribute('data-placeholder', 'true');
                
                // Insert at the correct position (after existing battles)
                const existingBattles = tableBattlesElement.children;
                if (existingBattles.length > currentBattleCount + index) {
                    tableBattlesElement.insertBefore(placeholder, existingBattles[currentBattleCount + index]);
                } else {
                    tableBattlesElement.appendChild(placeholder);
                }
                
                placeholders.push(placeholder);
            });
            
            // Force layout and measure positions (intentional unused read).
            // eslint-disable-next-line @typescript-eslint/no-unused-expressions
            tableBattlesElement.offsetHeight;
            
            placeholders.forEach((placeholder, index) => {
                const rect = placeholder.getBoundingClientRect();
                const position = {
                    x: rect.left + rect.width / 2,
                    y: rect.top + rect.height / 2
                };
                positions.set(`${index}`, position);
                // Cache this slot's geometry keyed by (table size, absolute slot
                // index, viewport) so future attacks at the same size skip the reflow.
                tableSlotPositionCache.set(tableSlotKey(totalSlots, currentBattleCount + index), position);
            });

            // Clean up placeholders
            placeholders.forEach(placeholder => placeholder.remove());
        }
        
        return positions;
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
            case 'flipped':
                return { x: 100, y: 180 }; // Slightly below the deck
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

        // target_card and battle_index are kept in the destructure for future
        // multi-card cover handling; currently unused.
        // eslint-disable-next-line @typescript-eslint/no-unused-vars
        const { type, cards, from_location, to_location, player_id, target_card, target_cards, battle_index, is_revert } = currentAnimation;

        // Handle magic_transition separately since it doesn't have cards
        if (type === 'magic_transition') {
            return; // Magic transitions are just messages, no visual animation needed
        }
        
        // All other animation types need cards
        if (!cards || cards.length === 0) {
            return;
        }

        // Check if cards are sanitized (refill from other players)
        const isSanitized = cards.every(card => card.suit === -1 && card.value === -1);

        // Small delay to ensure DOM is ready
        setTimeout(() => {
            // Measure placeholder positions for precise targeting
            const measuredPositions = measurePlaceholderPositions(type, cards, player_id);
            
            if (isSanitized) {
                // Render single CardBack for sanitized refill
                const sourceElement = findElementByLocation('deck');
                const destinationElement = findElementByLocation('hand', player_id);
                
                const startPos = sourceElement 
                    ? getElementPosition(sourceElement) 
                    : getFallbackPosition('deck');
                const endPos = destinationElement 
                    ? getElementPosition(destinationElement) 
                    : getFallbackPosition('hand', player_id);

                const newAnimatedCard: AnimatedCard = {
                    id: `sanitized-refill-${player_id}-${Date.now()}`,
                    card: { suit: -1, value: -1 }, // Keep original sanitized card
                    startPosition: startPos,
                    endPosition: endPos,
                    progress: 0,
                    animationType: type,
                    playerId: player_id,
                    isSanitizedRefill: true,
                    cardCount: cards.length,
                    isRevert: is_revert
                };

                setAnimatedCards([newAnimatedCard]);
            } else {
                // Render individual CardFaces for normal cards
                const newAnimatedCards: AnimatedCard[] = [];
                
                // For cover animations, keep track of which attack cards have been targeted
                const targetedAttackCards = new Set<string>();

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

                    // Find destination element - enhanced logic for multiple cards in one animation
                    let destinationElement: HTMLElement | null = null;
                    let endPos: { x: number; y: number };
                    
                    if (to_location === 'flipped') {
                        // First try to find the actual flipped card element
                        destinationElement = findElementByLocation('flipped');
                        if (destinationElement) {
                            endPos = getElementPosition(destinationElement);
                        } else {
                            // Fall back to positioning relative to deck
                            const deckElement = findElementByLocation('deck');
                            if (deckElement) {
                                const deckPos = getElementPosition(deckElement);
                                endPos = { x: deckPos.x, y: deckPos.y + 60 }; // 60px below deck
                            } else {
                                endPos = getFallbackPosition('flipped', player_id);
                            }
                        }
                    } else if (to_location === 'table') {
                        // Enhanced table targeting logic for multiple cards
                        if (type === 'cover') {
                            // For cover animations, find which attack card this cover card targets
                            let targetAttackCard = null;
                            
                            // If we have target_cards array (multi-card cover), use direct mapping
                            if (target_cards && target_cards[index]) {
                                targetAttackCard = target_cards[index];
                            } 
                            // Otherwise, use the game state and cover logic to determine the target
                            else if (game?.table_battles) {
                                // Find uncovered attack cards that this cover card can cover
                                const uncoveredBattles = game.table_battles.filter(battle => !battle.defense);
                                const powerSuit = game.power_suit;
                                
                                // Find the first uncovered attack card that this cover card can cover
                                // and hasn't already been targeted by another cover card in this animation
                                const targetBattle = uncoveredBattles.find(battle => {
                                    const cardKey = `${battle.attack.suit}-${battle.attack.value}`;
                                    return canCover(battle.attack, card, powerSuit) && !targetedAttackCards.has(cardKey);
                                });
                                
                                if (targetBattle) {
                                    targetAttackCard = targetBattle.attack;
                                    // Mark this attack card as targeted
                                    const cardKey = `${targetAttackCard.suit}-${targetAttackCard.value}`;
                                    targetedAttackCards.add(cardKey);
                                }
                            }
                            
                            if (targetAttackCard) {
                                // Try to find the specific attack card element
                                destinationElement = findElementByLocation('table', undefined, targetAttackCard.suit, targetAttackCard.value);
                                if (destinationElement) {
                                    endPos = getElementPosition(destinationElement);
                                } else {
                                    endPos = getFallbackPosition('table', player_id);
                                }
                            } else {
                                // Fallback: general table area with offset
                                destinationElement = findElementByLocation('table');
                                if (destinationElement) {
                                    endPos = getElementPosition(destinationElement);
                                    // Add offset for multiple cover cards
                                    const offset = index * 70; // 70px spacing between battles
                                    endPos.x += offset;
                                } else {
                                    endPos = getFallbackPosition('table', player_id);
                                }
                            }
                        } else if (type === 'attack_pass') {
                            // For attack/pass, use measured placeholder positions for precision
                            const measuredPos = measuredPositions.get(`${index}`);
                            
                            if (measuredPos) {
                                // Use the precisely measured position from invisible placeholder
                                endPos = measuredPos;
                            } else {
                                // Fallback to finding existing drop zones or general table position
                                const currentBattleCount = game?.table_battles?.length || 0;
                                const targetBattleIndex = currentBattleCount + index;
                                
                                destinationElement = findElementByLocation('table', undefined, undefined, undefined, targetBattleIndex);
                                if (destinationElement) {
                                    endPos = getElementPosition(destinationElement);
                                } else {
                                    endPos = getFallbackPosition('table', player_id);
                                    endPos.x += targetBattleIndex * 60;
                                }
                            }
                        } else {
                            // General table targeting (fallback)
                            destinationElement = findElementByLocation('table');
                            if (destinationElement) {
                                endPos = getElementPosition(destinationElement);
                            } else {
                                endPos = getFallbackPosition('table', player_id);
                            }
                        }
                    } else {
                        // Handle all other destination types
                        if (to_location === 'hand') {
                            destinationElement = findElementByLocation('hand', player_id);
                        } else if (to_location === 'discard') {
                            destinationElement = findElementByLocation('discard');
                        }

                        if (destinationElement) {
                            endPos = getElementPosition(destinationElement);
                        } else {
                            endPos = getFallbackPosition(to_location || 'table', player_id);
                        }
                    }

                    // Small offset for stacking if multiple cards go to same area
                    const stackOffset = index * 3;
                    endPos.x += stackOffset;
                    endPos.y += stackOffset;

                    newAnimatedCards.push({
                        id: `${card.suit}-${card.value}-${player_id}-${Date.now()}-${index}`,
                        card,
                        startPosition: startPos,
                        endPosition: endPos,
                        progress: 0,
                        animationType: type,
                        playerId: player_id,
                        isRevert: is_revert
                    });
                });

                setAnimatedCards(newAnimatedCards);
            }

            // Use CSS transitions - much smoother than manual animation
            // Set progress to 1 after a short delay to trigger the CSS transition. This
            // delay must still cross a browser paint (so the start frame renders before
            // the transition begins) but is kept tight so the per-event lifecycle fits
            // the queue's reduced inter-event gap (see below / AnimationContext).
            setTimeout(() => {
                setAnimatedCards(prev =>
                    prev.map(animatedCard => ({
                        ...animatedCard,
                        progress: 1 // This triggers the CSS transition
                    }))
                );
            }, 25);

            // Clear the overlay at ANIMATION_TIME. The AnimationContext queue advances
            // every ANIMATION_TIME + 25ms, and this clear is what would otherwise wipe
            // the NEXT event's freshly-created cards if it fired too late: clearing at
            // ANIMATION_TIME (rather than ANIMATION_TIME + 50) keeps it ~25ms ahead of
            // the next event's card creation. The visible glide is trimmed by only the
            // ~25ms transition-trigger delay above (the underlying card is already
            // committed at its destination by then), so the trim isn't noticeable.
            // These timings are matched to the queue gap — change them together.
            setTimeout(() => {
                setAnimatedCards([]);
            }, ANIMATION_TIME);
        }, 50); // Small delay to ensure DOM is ready (measurement reads committed state)

        // eslint-disable-next-line react-hooks/exhaustive-deps
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
                userSelect: 'none',
                WebkitUserSelect: 'none',
                WebkitTouchCallout: 'none',
            } as React.CSSProperties}
        >
            {animatedCards.map(animatedCard => {
                const { startPosition, endPosition, progress, card, id, isSanitizedRefill, cardCount, isRevert } = animatedCard;
                
                // Use actual position based on progress (CSS will animate the transition)
                const currentX = progress === 0 
                    ? startPosition.x 
                    : endPosition.x;
                const currentY = progress === 0 
                    ? startPosition.y 
                    : endPosition.y;

                return (
                    <div
                        key={id}
                        style={{
                            position: 'absolute',
                            left: currentX - 35, // Half card width
                            top: currentY - 45,  // Half card height
                            transform: `scale(${1.5 + progress * 0.3})`, // Scale up during animation
                            opacity: 1,
                            userSelect: 'none',
                            WebkitUserSelect: 'none',
                            WebkitTouchCallout: 'none',
                            // CSS transitions for smooth animation
                            transition: progress === 0 
                                ? 'none' // No transition for initial position
                                : `left ${ANIMATION_TIME}ms cubic-bezier(0.25, 0.46, 0.45, 0.94), top ${ANIMATION_TIME}ms cubic-bezier(0.25, 0.46, 0.45, 0.94), transform ${ANIMATION_TIME}ms ease-out`
                        } as React.CSSProperties}
                    >
                        {isSanitizedRefill ? (
                            <CardBack 
                                deckSize={cardCount || 1}
                                enableRandomRotation={false}
                            />
                        ) : (
                            <CardFace 
                                card={card}
                                isAnimationOverlay={true}
                                style={{
                                    border: isRevert 
                                        ? '2px solid rgb(220, 38, 38)' 
                                        : '2px solid black',
                                    boxShadow: isRevert 
                                        ? `0 ${progress * 10}px ${progress * 20}px rgba(255,0,0,0.6)` 
                                        : `0 ${progress * 10}px ${progress * 20}px rgba(0,0,0,0.4)`,
                                    filter: isRevert 
                                        ? 'brightness(1.3) contrast(1.2) sepia(0.3) saturate(1.8) hue-rotate(-10deg)'
                                        : 'none',
                                    backgroundColor: isRevert ? 'rgb(255, 150, 150)' : 'var(--color-card-face)'
                                }}
                            />
                        )}
                    </div>
                );
            })}
        </div>
    );
}; 