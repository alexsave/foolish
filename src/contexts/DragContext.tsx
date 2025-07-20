import React, { createContext, useContext, useEffect, useRef, useState } from 'react';
import { Card, PersonalGame } from '../common/types';
import { useServer } from './ServerContext';
import { useAnimation } from './AnimationContext';
import { useAuth } from './AuthContext';
import { useGame } from './GameContext';
import { canCover } from '../common/common_utils';

const DragContext = createContext<DragContextType | null>(null);

export const DragProvider = ({ children }: { children: React.ReactNode }) => {
    const { user_id } = useAuth();
    const game: PersonalGame = useServer().game as PersonalGame;
    const { rearrangeHand, localHandOrder, setLocalHandOrder } = useServer();
    const { attack, pass, cover } = useAnimation();

    const { selectedCards, setSelectedCards, handleCardSelection } = useGame();

    const [draggedCardIndex, setDraggedCardIndex] = useState<number | null>(null);
    const [isDraggingCard, setIsDraggingCard] = useState(false);
    const [dragStartPos, setDragStartPos] = useState<{ x: number; y: number } | null>(null);
    const [hasSwapped, setHasSwapped] = useState(false);
    const [isActuallyDragging, setIsActuallyDragging] = useState(false);
    const [touchStartTime, setTouchStartTime] = useState<number>(0);

    // New state for game action drag detection
    const [isDraggingForGameAction, setIsDraggingForGameAction] = useState(false);
    const [draggedCard, setDraggedCard] = useState<Card | null>(null);
    const [currentCursorPos, setCurrentCursorPos] = useState<{ x: number; y: number } | null>(null);

    // Cleanup timer on unmount
    useEffect(() => {
        return () => {
            if (rearrangeCardTimerRef.current) {
                clearTimeout(rearrangeCardTimerRef.current);
            }
        };
    }, []);

    // Helper function to detect if drag is in the hand area
    const isInHandArea = (x: number, y: number) => {
        const handAreaTop = window.innerHeight - 150; // Hand area starts 200px from bottom
        return y >= handAreaTop;
    };

    // Helper function to find all valid cover combinations for multiple cards
    const findCoverCombinations = (coverCards: Card[], uncoveredAttacks: Card[]): { coverCards: Card[], attackCards: Card[] }[] => {
        const combinations: { coverCards: Card[], attackCards: Card[] }[] = [];
        
        if (coverCards.length === 0 || uncoveredAttacks.length === 0) {
            return combinations;
        }

        // For each permutation of uncovered attacks (taking coverCards.length items)
        const generatePermutations = (arr: Card[], length: number): Card[][] => {
            if (length === 1) return arr.map(item => [item]);
            
            const result: Card[][] = [];
            for (let i = 0; i < arr.length; i++) {
                const rest = arr.slice(0, i).concat(arr.slice(i + 1));
                const subPermutations = generatePermutations(rest, length - 1);
                for (const subPerm of subPermutations) {
                    result.push([arr[i], ...subPerm]);
                }
            }
            return result;
        };

        // Only consider combinations where we have the right number of cards
        if (coverCards.length <= uncoveredAttacks.length) {
            const attackPermutations = generatePermutations(uncoveredAttacks, coverCards.length);
            
            for (const attackPerm of attackPermutations) {
                // Check if this cover-attack pairing is valid
                const isValidCombination = coverCards.every((coverCard, index) => 
                    canCover(attackPerm[index], coverCard, game.power_suit)
                );
                
                if (isValidCombination) {
                    combinations.push({
                        coverCards: [...coverCards],
                        attackCards: [...attackPerm]
                    });
                }
            }
        }

        return combinations;
    };

    // Helper function to check if a multi-card cover is unambiguous
    const findUnambiguousCover = (cardsToUse: Card[]): { coverCards: Card[], attackCards: Card[] } | null => {
        const uncoveredBattles = game.table_battles.filter(battle => !battle.defense);
        const uncoveredAttacks = uncoveredBattles.map(battle => battle.attack);
        
        const validCombinations = findCoverCombinations(cardsToUse, uncoveredAttacks);
        
        if (validCombinations.length === 0) {
            return null;
        }
        
        // Check if all valid combinations result in the same set of attack cards being covered
        const cardToString = (card: Card) => `${card.value}-${card.suit}`;
        const firstCombinationAttackSet = new Set(validCombinations[0].attackCards.map(cardToString));
        
        const allCombinationsHaveSameAttackSet = validCombinations.every(combo => {
            const comboAttackSet = new Set(combo.attackCards.map(cardToString));
            return comboAttackSet.size === firstCombinationAttackSet.size && 
                   Array.from(comboAttackSet).every(cardStr => firstCombinationAttackSet.has(cardStr));
        });
        
        // If all combinations cover the same set of attack cards, it's unambiguous
        if (allCombinationsHaveSameAttackSet) {
            return validCombinations[0]; // Return any valid combination since they all cover the same attacks
        }
        
        return null;
    };

    // Helper function to determine what action should be taken
    const determineGameAction = (x: number, y: number, draggedCard: Card) => {
        if (isInHandArea(x, y)) {
            return { type: 'rearrange' as const };
        }

        const self_index = game.players.findIndex((player) => player.player_id === user_id);
        const isDefending = game.defender === self_index;

        if (isDefending) {
            const tableCardUnderCursor = getTableCardUnderCursor(x, y);
            const passIsPossible = canPass(draggedCard);

            // Check if the dragged card is part of selected cards
            const isDraggedCardSelected = selectedCards.some(selectedCard =>
                selectedCard.value === draggedCard.value && selectedCard.suit === draggedCard.suit
            );

            // Use all selected cards if the dragged card is selected, otherwise just the dragged card
            const cardsToUse = isDraggedCardSelected && selectedCards.length > 0 ? selectedCards : [draggedCard];

            if (tableCardUnderCursor && !tableCardUnderCursor.defense) {
                // Dragging to an uncovered attack card
                if (cardsToUse.length === 1) {
                    // Single card cover - existing logic
                    return { type: 'cover' as const, targetCard: tableCardUnderCursor.attack };
                } else {
                    // Multi-card cover - check if unambiguous
                    const unambiguousCover = findUnambiguousCover(cardsToUse);
                    if (unambiguousCover) {
                        return { type: 'multicover' as const, coverCards: unambiguousCover.coverCards, attackCards: unambiguousCover.attackCards };
                    } else {
                        return { type: 'invalid' as const };
                    }
                }
            } else if (!passIsPossible) {
                // Can't pass and in empty space
                if (cardsToUse.length === 1) {
                    // Single card - existing logic
                    const uncoveredBattles = game.table_battles.filter(battle => !battle.defense);
                    if (uncoveredBattles.length === 1) {
                        return { type: 'cover' as const, targetCard: uncoveredBattles[0].attack };
                    } else {
                        return { type: 'invalid' as const };
                    }
                } else {
                    // Multi-card cover - check if unambiguous
                    const unambiguousCover = findUnambiguousCover(cardsToUse);
                    if (unambiguousCover) {
                        return { type: 'multicover' as const, coverCards: unambiguousCover.coverCards, attackCards: unambiguousCover.attackCards };
                    } else {
                        return { type: 'invalid' as const };
                    }
                }
            } else {
                // Pass is possible and dragging to empty space = pass
                return { type: 'pass' as const };
            }
        } else {
            // Attacker: dragging to table = attack
            return { type: 'attack' as const };
        }
    };

    // Helper function to detect which table card is under the cursor
    const getTableCardUnderCursor = (x: number, y: number) => {
        const elements = document.elementsFromPoint(x, y);
        const battleCardElement = elements.find(el => el.getAttribute('data-battle-index') !== null);
        if (battleCardElement) {
            const battleIndex = parseInt(battleCardElement.getAttribute('data-battle-index')!);
            return game.table_battles[battleIndex];
        }
        return null;
    };

    // Helper function to check if passing is possible
    const canPass = (draggedCard: Card) => {
        const table_battles = game.table_battles;
        if (table_battles.length === 0) return false;

        // Check if the dragged card is part of selected cards
        const isDraggedCardSelected = selectedCards.some(selectedCard =>
            selectedCard.value === draggedCard.value && selectedCard.suit === draggedCard.suit
        );

        // Use all selected cards if the dragged card is selected, otherwise just the dragged card
        const cardsToCheck = isDraggedCardSelected && selectedCards.length > 0 ? selectedCards : [draggedCard];

        // All cards must have the same value
        if (!cardsToCheck.every(card => card.value === cardsToCheck[0].value)) {
            return false;
        }

        // All table battles must be uncovered (defense === null)
        // All uncovered attacks must have the same value as the cards to check
        return table_battles.every(battle =>
            battle.defense === null && battle.attack.value === cardsToCheck[0].value
        );
    };

    const startCardDrag = (e: React.MouseEvent | React.TouchEvent, index: number) => {
        e.preventDefault();
        e.stopPropagation();

        // Cancel any pending rearrange update since user is still actively dragging
        if (rearrangeCardTimerRef.current) {
            clearTimeout(rearrangeCardTimerRef.current);
            rearrangeCardTimerRef.current = null;
        }

        const clientX = 'touches' in e ? e.touches[0].clientX : e.clientX;
        const clientY = 'touches' in e ? e.touches[0].clientY : e.clientY;

        setIsDraggingCard(true);
        setDraggedCardIndex(index);
        setDragStartPos({ x: clientX, y: clientY });
        setHasSwapped(false);
        setIsActuallyDragging(false);
        setTouchStartTime(Date.now());
    };


    useEffect(() => {
        const handleMouseMove = (e: MouseEvent | Touch) => {
            if (!(isDraggingCard && draggedCardIndex !== null)) {
                return;
            }

            // Update current cursor position
            setCurrentCursorPos({ x: e.clientX, y: e.clientY });

            // Check if we've moved far enough to consider this actual dragging
            if (dragStartPos && !isActuallyDragging) {
                const distance = Math.sqrt(
                    Math.pow(e.clientX - dragStartPos.x, 2) +
                    Math.pow(e.clientY - dragStartPos.y, 2)
                );
                if (distance > 10) { // 10 pixels threshold for actual dragging
                    setIsActuallyDragging(true);
                }
            }

            // Check if dragging outside hand area for game actions
            if (isActuallyDragging && !isInHandArea(e.clientX, e.clientY)) {
                if (!isDraggingForGameAction) {
                    setIsDraggingForGameAction(true);
                    setDraggedCard(localHandOrder[draggedCardIndex]);
                }
            } else if (isDraggingForGameAction && isInHandArea(e.clientX, e.clientY)) {
                // Dragged back into hand area
                setIsDraggingForGameAction(false);
                setDraggedCard(null);
            }

            // Only allow swaps when actually dragging and still in hand area
            if (isActuallyDragging && !isDraggingForGameAction) {
                // Find what card we're hovering over for real-time swapping
                const elements = document.elementsFromPoint(e.clientX, e.clientY);
                const cardElement = elements.find(el => el.getAttribute('data-card-index') !== null);
                if (!cardElement) {
                    return;
                }
                const targetIndex = parseInt(cardElement.getAttribute('data-card-index')!);
                if (targetIndex === draggedCardIndex) {
                    return;
                }
                // Do immediate swap in the array
                const newOrder = [...localHandOrder];
                const draggedCard = newOrder[draggedCardIndex];
                const targetCard = newOrder[targetIndex];

                // Swap the cards
                newOrder[draggedCardIndex] = targetCard;
                newOrder[targetIndex] = draggedCard;

                setLocalHandOrder(newOrder);
                setDraggedCardIndex(targetIndex); // Update dragged index to new position
                setHasSwapped(true); // Mark that a swap occurred
            }
        };

        const handleTouchMove = (e: TouchEvent) => {

            if (!(isDraggingCard && draggedCardIndex !== null && e.touches.length > 0)) {
                return;
            }

            e.preventDefault();
            const touch = e.touches[0];

            handleMouseMove(touch);
        };

        const handleEnd = (e: MouseEvent | TouchEvent) => {
            if (isDraggingCard) {
                e.preventDefault();
                endCardDrag();
            }
        };

        if (isDraggingCard) {
            document.addEventListener('mousemove', handleMouseMove);
            document.addEventListener('touchmove', handleTouchMove, { passive: false });
            document.addEventListener('mouseup', handleEnd);
            document.addEventListener('touchend', handleEnd);
        }

        return () => {
            document.removeEventListener('mousemove', handleMouseMove);
            document.removeEventListener('touchmove', handleTouchMove);
            document.removeEventListener('mouseup', handleEnd);
            document.removeEventListener('touchend', handleEnd);
        };
    }, [isDraggingCard, draggedCardIndex, localHandOrder, hasSwapped, isActuallyDragging, touchStartTime, isDraggingForGameAction, draggedCard, currentCursorPos]);

    const endCardDrag = () => {
        if (!isDraggingCard || draggedCardIndex === null) return;

        const touchDuration = Date.now() - touchStartTime;
        const wasTap = touchDuration < 150 && !hasSwapped && !isActuallyDragging;

        // If this was a tap (not a drag), handle card selection
        if (wasTap && localHandOrder[draggedCardIndex]) {
            handleCardSelection(localHandOrder[draggedCardIndex]);
        }

        // Handle game actions if dragging ended outside hand area
        if (isDraggingForGameAction && draggedCard && isActuallyDragging && currentCursorPos) {
            const action = determineGameAction(currentCursorPos.x, currentCursorPos.y, draggedCard);

            // Check if the dragged card is part of selected cards
            const isDraggedCardSelected = selectedCards.some(selectedCard =>
                selectedCard.value === draggedCard.value && selectedCard.suit === draggedCard.suit
            );

            // Use all selected cards if the dragged card is selected, otherwise just the dragged card
            const cardsToUse = isDraggedCardSelected && selectedCards.length > 0 ? selectedCards : [draggedCard];

            if (action.type === 'attack') {
                attack(cardsToUse).then(() => {
                    console.log('Attack performed via drag');
                    setSelectedCards([]); // Clear selection after successful action
                }).catch((e) => {
                    console.error('Attack failed:', e.message);
                });

            } else if (action.type === 'cover' && action.targetCard) {
                // Single card cover
                const cardToUse = cardsToUse[0];
                cover([cardToUse], [action.targetCard]).then(() => {
                    console.log('Cover performed via drag');
                    setSelectedCards([]); // Clear selection after successful action
                }).catch((e) => {
                    console.error('Cover failed:', e.message);
                });

            } else if (action.type === 'multicover' && action.coverCards && action.attackCards) {
                // Multi-card cover with unambiguous mapping
                cover(action.coverCards, action.attackCards).then(() => {
                    console.log('Multi-card cover performed via drag');
                    setSelectedCards([]); // Clear selection after successful action
                }).catch((e) => {
                    console.error('Multi-card cover failed:', e.message);
                });

            } else if (action.type === 'pass') {

                pass(cardsToUse).then(() => {
                    console.log('Pass performed via drag');
                    setSelectedCards([]); // Clear selection after successful action
                }).catch((e) => {
                    console.error('Pass failed:', e.message);
                });
            }

        }

        // Schedule the final update to the server with current order (for rearranging)
        if (hasSwapped) {
            scheduleCardRearrangeUpdate(localHandOrder);
        }

        setIsDraggingCard(false);
        setDraggedCardIndex(null);
        setDragStartPos(null);
        setIsActuallyDragging(false);
        setIsDraggingForGameAction(false);
        setDraggedCard(null);
        setCurrentCursorPos(null);

        // Reset the swap flag after a short delay to prevent immediate click
        setTimeout(() => {
            setHasSwapped(false);
        }, 100);
    };

    const rearrangeCardTimerRef = useRef<NodeJS.Timeout | null>(null);
    const scheduleCardRearrangeUpdate = (newOrder: Card[]) => {
        // Cancel existing timer
        if (rearrangeCardTimerRef.current) {
            clearTimeout(rearrangeCardTimerRef.current);
        }

        // Create indices array based on original order
        const originalHand = game.self.hand || [];
        const indices = newOrder.map(newCard =>
            originalHand.findIndex(origCard =>
                origCard.value === newCard.value && origCard.suit === newCard.suit
            )
        );

        // Set new 6-second timer
        rearrangeCardTimerRef.current = setTimeout(() => {
            rearrangeHand(game.id!, indices).catch(error => {
                console.error('Failed to rearrange hand:', error);
                // Revert to original order on error
                setLocalHandOrder(originalHand);
            });
        }, 5000);
    };



    return (
        <DragContext.Provider value={{
            draggedCardIndex,
            isActuallyDragging,
            isDraggingForGameAction,
            draggedCard,
            currentCursorPos,
            determineGameAction,
            startCardDrag
        }}>
            {children}
        </DragContext.Provider>
    );
};

interface DragContextType {
    draggedCardIndex: number | null;
    isActuallyDragging: boolean;
    isDraggingForGameAction: boolean;
    draggedCard: Card | null;
    currentCursorPos: { x: number; y: number } | null;
    determineGameAction: (x: number, y: number, draggedCard: Card) => 
        | { type: 'attack' }
        | { type: 'cover', targetCard: Card }
        | { type: 'multicover', coverCards: Card[], attackCards: Card[] }
        | { type: 'pass' }
        | { type: 'rearrange' }
        | { type: 'invalid' };
    startCardDrag: (e: React.MouseEvent | React.TouchEvent, index: number) => void;
}


export const useDrag = () => {
    const context = useContext(DragContext);
    if (!context) {
        throw new Error('useAuth must be used within a AuthProvider');
    }
    return context;
}; 