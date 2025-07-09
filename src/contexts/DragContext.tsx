import React, { createContext, useContext, useEffect, useRef, useState } from 'react';
import { Card, PersonalGame } from '../common/types';
import { useServer } from './ServerContext';
import { useAuth } from './AuthContext';
import { useGame } from './GameContext';

const DragContext = createContext<DragContextType | null>(null);

export const DragProvider = ({ children }: { children: React.ReactNode }) => {
    const { user_id } = useAuth();
    const game: PersonalGame = useServer().game as PersonalGame;
    const { attack, pass, cover, rearrangeHand } = useServer();

    const { selectedCards, setSelectedCards, localHandOrder, setLocalHandOrder, handleCardSelection } = useGame();

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

    // Helper function to determine what action should be taken
    const determineGameAction = (x: number, y: number, draggedCard: Card) => {
        if (isInHandArea(x, y)) {
            return { type: 'rearrange' };
        }

        const self_index = game.players.findIndex((player) => player.player_id === user_id);
        const isDefending = game.defender === self_index;

        if (isDefending) {
            const tableCardUnderCursor = getTableCardUnderCursor(x, y);
            const passIsPossible = canPass(draggedCard);

            if (tableCardUnderCursor && !tableCardUnderCursor.defense) {
                // Dragging to an uncovered attack card = cover
                return { type: 'cover', targetCard: tableCardUnderCursor.attack };
            } else if (!passIsPossible) {
                // Can't pass and in empty space - check if there's only one card to cover
                const uncoveredBattles = game.table_battles.filter(battle => !battle.defense);
                if (uncoveredBattles.length === 1) {
                    // Auto-cover the single uncovered card
                    return { type: 'cover', targetCard: uncoveredBattles[0].attack };
                } else {
                    // Multiple uncovered cards or no uncovered cards - no valid action
                    return { type: 'invalid' };
                }
            } else {
                // Pass is possible and dragging to empty space = pass
                return { type: 'pass' };
            }
        } else {
            // Attacker: dragging to table = attack
            return { type: 'attack' };
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
                // For cover, we can only cover one card at a time, so use just the first card
                // But we could extend this later to handle multiple covers
                const cardToUse = cardsToUse[0];
                cover([cardToUse], [action.targetCard]).then(() => {
                    console.log('Cover performed via drag');
                    setSelectedCards([]); // Clear selection after successful action
                }).catch((e) => {
                    console.error('Cover failed:', e.message);
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
    determineGameAction: (x: number, y: number, draggedCard: Card) => { type: string, targetCard?: Card } | { type: 'invalid' };
    startCardDrag: (e: React.MouseEvent | React.TouchEvent, index: number) => void;
}


export const useDrag = () => {
    const context = useContext(DragContext);
    if (!context) {
        throw new Error('useAuth must be used within a AuthProvider');
    }
    return context;
}; 