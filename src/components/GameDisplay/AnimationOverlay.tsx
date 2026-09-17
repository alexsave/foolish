import { useEffect, useLayoutEffect, useState, useRef } from 'react';
import { ANIMATION_TIME, useAnimation } from '../../contexts/AnimationContext';
import { covered, seatKey, type ViewCard as Card } from '../../state/view';
import { CardFace } from './CardFace';
import { CardBack } from './CardBack';
import { useServer } from '../../contexts/ServerContext';
// The kernel's can_cover: bots.wasm is loaded before any screen renders
// (src/app/providers.tsx KernelGate), the replay and the tutorial included.
import { canCoverPair } from '../../utils/gameValidation';

// Table-slot geometry cache (Stage 9). The on-table battle layout is a function of
// only (how many battle slots there are, the viewport size) - the 4th slot in a
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

// Where each card's last flight to the table landed, by card. A card whose move the
// server refused lands on the table in flight only - no board ever lays it - so its
// return flight starts from the spot the outbound flight left it at.
const tableLandings = new Map<string, { x: number; y: number }>();

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
    flight: object; // the event this card flies for
    fromLanding?: boolean; // starts where an earlier flight landed, at that flight's landing scale
}

export const AnimationOverlay = () => {
    const [animatedCards, setAnimatedCards] = useState<AnimatedCard[]>([]);
    const { currentAnimation, isAnimating } = useAnimation();
    const { view: game } = useServer();
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
    const findElementByLocation = (location: string, playerId?: string, cardSuit?: number, cardValue?: number, battleIndex?: number): HTMLElement | null => {
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
            
            // If we have specific card coordinates, try to find that card ON THE TABLE
            // (the same card in a hand is not where a table flight starts or ends)
            if (cardSuit !== undefined && cardValue !== undefined) {
                const cardSelector = `[data-location="table"] [data-card="${cardSuit}-${cardValue}"]`;
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
        
        return document.querySelector(generalSelector) as HTMLElement | null;
    };

    // Helper function to create invisible placeholders and measure their positions
    const measurePlaceholderPositions = (type: string, cards: readonly Card[], player_id?: string): Map<string, { x: number; y: number }> => {
        const positions = new Map<string, { x: number; y: number }>();

        if (type === 'attack_pass') {
            const currentBattleCount = game?.battles.length || 0;
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

            // Find the table battles container. The container itself is tagged
            // (data-table-container) so this also works when the table is EMPTY -
            // an opponent's first attack of a bout used to find no
            // [data-location="table"] child at all and fall back to a generic
            // center position instead of the real first slot.
            const tableBattlesElement = (document.querySelector('[data-table-container]')
                ?? document.querySelector('[data-location="table"]')?.parentElement) as HTMLElement | null;
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

    // Measure where cards ENTERING the local player's hand will actually land.
    // The rendered hand appends new cards at the END (displayedHand), but the
    // old targeting picked querySelector's FIRST hand-card match - so drawn
    // cards flew toward the leftmost card instead of their landing slot. Same
    // placeholder trick as the table slots: append invisible flex items with a
    // real card's flex geometry, reflow, measure, remove - the measured spots
    // include the squeeze the incoming cards cause. Returns [] for players
    // without a per-card hand in the DOM (opponents' mini-hands).
    const measureHandSlotPositions = (count: number, playerId?: string): { x: number; y: number }[] => {
        if (!playerId) return [];
        const container = document.querySelector(`[data-hand-container][data-player-id="${playerId}"]`) as HTMLElement | null;
        if (!container) return [];

        const placeholders: HTMLElement[] = [];
        for (let i = 0; i < count; i++) {
            const ph = document.createElement('div');
            // Same flex-item geometry as a hand card (ActionButtons): the
            // measured layout matches the hand once the drawn cards commit.
            ph.style.cssText = 'flex:1 1 0;min-width:20px;max-width:50px;height:70px;visibility:hidden;pointer-events:none;';
            ph.setAttribute('data-placeholder', 'true');
            container.appendChild(ph);
            placeholders.push(ph);
        }
        // Force layout and measure (intentional unused read).
        // eslint-disable-next-line @typescript-eslint/no-unused-expressions
        container.offsetHeight;
        const out = placeholders.map(ph => {
            const rect = ph.getBoundingClientRect();
            return { x: rect.left + rect.width / 2, y: rect.top + rect.height / 2 };
        });
        placeholders.forEach(ph => ph.remove());
        return out;
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

    // Built in a layout effect, before the browser paints the commit that started
    // the flight: that commit hides the card where it stands (CardFace), and the
    // flight must show it in the same frame. The flight lives exactly as long as its
    // event: the commit that lands it (AnimationContext) takes it off the overlay in
    // the frame the board shows the card where it landed.
    useLayoutEffect(() => {
        if (!currentAnimation || !isAnimating) {
            setAnimatedCards((prev) => (prev.length === 0 ? prev : []));
            return;
        }

        // target_card and battle_index are kept in the destructure for future
        // multi-card cover handling; currently unused.
        // eslint-disable-next-line @typescript-eslint/no-unused-vars
        const { type, cards, from_location, to_location, seat, target_card, target_cards, battle_index, is_revert } = currentAnimation;
        // The page names a seat's hand by its player id (data-player-id): the id the
        // event's own board gives its seat, or the board on screen's for a flight
        // that carries none (a move of mine, a revert).
        const eventBoard = currentAnimation.game_state ?? game;
        const player_id = seat === undefined ? undefined : seatKey(eventBoard, seat);

        // Magic transitions are just messages, and every other type needs cards
        if (type === 'magic_transition' || !cards || cards.length === 0) {
            setAnimatedCards((prev) => (prev.length === 0 ? prev : []));
            return;
        }

        // Check if cards are sanitized (refill from other players)
        const isSanitized = cards.every(card => card.suit === -1 && card.value === -1);

        {
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
                    isRevert: is_revert,
                    flight: currentAnimation,
                };

                setAnimatedCards([newAnimatedCard]);
            } else {
                // Render individual CardFaces for normal cards
                const newAnimatedCards: AnimatedCard[] = [];

                // Cards entering the local hand land at its END - measure those
                // slots once for the whole batch (deal/refill/pickup).
                const handSlots = to_location === 'hand'
                    ? measureHandSlotPositions(cards.length, player_id)
                    : [];

                // For cover animations, keep track of which attack cards have been targeted
                const targetedAttackCards = new Set<string>();

                cards.forEach((card, index) => {
                    // Find source element
                    let sourceElement: HTMLElement | null = null;
                    let startPos: { x: number; y: number };
                    
                    let remembered: { x: number; y: number } | undefined;
                    if (from_location === 'hand') {
                        sourceElement = findElementByLocation('hand', player_id, card.suit, card.value);
                    } else if (from_location === 'deck') {
                        sourceElement = findElementByLocation('deck');
                    } else if (from_location === 'table') {
                        sourceElement = document.querySelector(`[data-location="table"] [data-card="${card.suit}-${card.value}"]`) as HTMLElement | null;
                        remembered = sourceElement ? undefined : tableLandings.get(`${card.suit}-${card.value}`);
                        if (!sourceElement && !remembered) sourceElement = findElementByLocation('table', undefined, card.suit, card.value);
                    }

                    if (sourceElement) {
                        startPos = getElementPosition(sourceElement);
                    } else if (remembered) {
                        startPos = { ...remembered };
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
                            else if (game?.battles) {
                                // Find uncovered attack cards that this cover card can cover
                                const uncoveredBattles = game.battles.filter(battle => !covered(battle));
                                const powerSuit = game.powerSuit;
                                
                                // Find the first uncovered attack card that this cover card can cover
                                // and hasn't already been targeted by another cover card in this animation
                                const targetBattle = uncoveredBattles.find(battle => {
                                    const cardKey = `${battle.attack.suit}-${battle.attack.value}`;
                                    return canCoverPair(battle.attack, card, powerSuit) && !targetedAttackCards.has(cardKey);
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
                            // For attack/pass, use measured placeholder positions for precision,
                            // unless the board already shows the card (a confirmation that
                            // beat its flight): then that card is where it lands.
                            const measuredPos = measuredPositions.get(`${index}`);
                            const standing = document.querySelector(`[data-location="table"] [data-card="${card.suit}-${card.value}"]`) as HTMLElement | null;

                            if (standing) {
                                endPos = getElementPosition(standing);
                            } else if (measuredPos) {
                                // Use the precisely measured position from invisible placeholder
                                endPos = measuredPos;
                            } else {
                                // Fallback to finding existing drop zones or general table position
                                const currentBattleCount = game?.battles.length || 0;
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
                            // The card's own place when the hand already holds it (a
                            // refused card never left the board's hand; a board committed
                            // before the flight shows it there), else the precisely
                            // measured landing slot for the local hand; opponents'
                            // mini-hands fall through to their container.
                            const own = player_id
                                ? document.querySelector(`[data-location="hand"][data-player-id="${player_id}"][data-card="${card.suit}-${card.value}"]`) as HTMLElement | null
                                : null;
                            const slot = handSlots[index];
                            if (own) {
                                endPos = getElementPosition(own);
                            } else if (slot) {
                                endPos = slot;
                            } else {
                                destinationElement = findElementByLocation('hand', player_id);
                                endPos = destinationElement
                                    ? getElementPosition(destinationElement)
                                    : getFallbackPosition('hand', player_id);
                            }
                        } else {
                            if (to_location === 'discard') {
                                destinationElement = findElementByLocation('discard');
                            }
                            endPos = destinationElement
                                ? getElementPosition(destinationElement)
                                : getFallbackPosition(to_location || 'table', player_id);
                        }
                    }

                    // Small offset so simultaneous cards into the same UNMEASURED
                    // area don't fully overlap; measured targets (table slots, hand
                    // slots) are exact - offsetting them would re-introduce drift.
                    const preciselyMeasured = (to_location === 'hand' && handSlots[index] !== undefined) ||
                        (type === 'attack_pass' && measuredPositions.get(`${index}`) !== undefined);
                    if (!preciselyMeasured) {
                        const stackOffset = index * 3;
                        endPos.x += stackOffset;
                        endPos.y += stackOffset;
                    }

                    if (to_location === 'table') tableLandings.set(`${card.suit}-${card.value}`, { ...endPos });

                    newAnimatedCards.push({
                        fromLanding: !sourceElement && !!remembered,
                        id: `${card.suit}-${card.value}-${player_id}-${Date.now()}-${index}`,
                        card,
                        startPosition: startPos,
                        endPosition: endPos,
                        progress: 0,
                        animationType: type,
                        playerId: player_id,
                        isRevert: is_revert,
                        flight: currentAnimation,
                    });
                });

                setAnimatedCards(newAnimatedCards);
            }

        }

        // Use CSS transitions - much smoother than manual animation. Progress goes to 1
        // after a short delay that crosses a browser paint, so the start frame renders
        // before the transition begins; only this flight's cards move.
        const flight = currentAnimation;
        const begin = setTimeout(() => {
            setAnimatedCards(prev => prev.map(animatedCard => (animatedCard.flight === flight
                ? { ...animatedCard, progress: 1 } // This triggers the CSS transition
                : animatedCard)));
        }, 25);
        return () => clearTimeout(begin);

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
                const { startPosition, endPosition, progress, card, id, isSanitizedRefill, cardCount, isRevert, fromLanding } = animatedCard;
                
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
                            // Scale up during animation; a card taking off from where a flight
                            // left it starts at the size that flight landed at
                            transform: `scale(${progress === 0 && fromLanding ? 1.8 : 1.5 + progress * 0.3})`,
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