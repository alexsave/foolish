import { Card, PersonalGame } from '../common/types';
import React, { useState, useEffect, useRef } from 'react';
import { useServer } from '../contexts/ServerContext';
import { useAuth } from '../contexts/AuthContext';
import { generateCardBackPattern } from '../utils/cards';
import { TableBattles } from './GameDisplay/TableBattles';
import { PlayerRing } from './GameDisplay/PlayerRing';
import { DefenderShield } from './GameDisplay/DefenderShield';
import { ActionButtons } from './GameDisplay/ActionButtons';
import { DeckAndFlipped } from './GameDisplay/DeckAndFlipped';
import { DragShadow } from './GameDisplay/DragShadow';
import { CoverArrows } from './GameDisplay/CoverArrows';

export const GameDisplay = () => {
  const { user_id } = useAuth();
  const { game, attack, pass, pickup, cover, good, rearrangeHand } = useServer();
  const state = game as PersonalGame;
  const [selectedCards, setSelectedCards] = useState<Card[]>([]);

  const [coverMap, setCoverMap] = useState<Map<Card, Card>>(new Map());

  // Enhanced drag and drop state for cards
  const [draggedCardIndex, setDraggedCardIndex] = useState<number | null>(null);
  const [isDraggingCard, setIsDraggingCard] = useState(false);
  const [localHandOrder, setLocalHandOrder] = useState<Card[]>([]);
  const [dragStartPos, setDragStartPos] = useState<{ x: number; y: number } | null>(null);
  const [hasSwapped, setHasSwapped] = useState(false);
  const [isActuallyDragging, setIsActuallyDragging] = useState(false);
  const [touchStartTime, setTouchStartTime] = useState<number>(0);
  const rearrangeCardTimerRef = useRef<NodeJS.Timeout | null>(null);

  // New state for game action drag detection
  const [isDraggingForGameAction, setIsDraggingForGameAction] = useState(false);
  const [draggedCard, setDraggedCard] = useState<Card | null>(null);
  const [currentCursorPos, setCurrentCursorPos] = useState<{ x: number; y: number } | null>(null);

  // Generate pattern for smaller player cards
  const [playerCardPatternDataUrl, setPlayerCardPatternDataUrl] = useState<string>('');

  useEffect(() => {
    // Generate the pattern for player cards once when component mounts
    const dataUrl = generateCardBackPattern(12, 18);
    setPlayerCardPatternDataUrl(dataUrl);
  }, []);

  const handleCardSelection = (card: Card) => {
    const isSelected = selectedCards.some(selectedCard =>
      selectedCard.value === card.value && selectedCard.suit === card.suit
    );

    if (isSelected) {
      setSelectedCards(selectedCards.filter(c => !(c.value === card.value && c.suit === card.suit)));
    } else {
      setSelectedCards([...selectedCards, card]);
    }
  };

  // Game loading is now handled by ServerContext

  // Update local hand order when game changes
  useEffect(() => {
    if (state?.self?.hand) {
      setLocalHandOrder(state.self.hand);
    }
  }, [state?.self?.hand]);

  // Game loading state is now handled by ServerContext

  // Cleanup timer on unmount
  useEffect(() => {
    return () => {
      if (rearrangeCardTimerRef.current) {
        clearTimeout(rearrangeCardTimerRef.current);
      }
    };
  }, []);

  // Prevent page scrolling/dragging during touch interactions but allow legitimate drags
  useEffect(() => {
    const preventPageScroll = (e: TouchEvent) => {
      const target = e.target as HTMLElement;

      // Allow dragging on draggable elements (cards)
      if (target.closest('[draggable="true"]')) {
        return; // Don't prevent - allow legitimate drag
      }

      // Allow interactions on interactive elements (buttons, inputs, or anything with higher z-index)
      if (target.tagName === 'BUTTON' ||
        target.tagName === 'INPUT' ||
        target.closest('input') ||
        window.getComputedStyle(target).zIndex === '1000') {
        return; // Don't prevent - allow button clicks and input focus
      }

      // Prevent page scrolling/panning on background/text areas
      if (e.touches.length > 1 || (e.touches.length === 1 && e.type === 'touchmove')) {
        e.preventDefault();
      }
    };

    // Only prevent touchmove to avoid interfering with clicks/taps
    document.addEventListener("touchmove", preventPageScroll, { passive: false });

    // Cleanup function to remove event listeners
    return () => {
      document.removeEventListener("touchmove", preventPageScroll);
    };
  }, []);

  // we chose a card to cover WITH, now we choose WHICH card to cover
  const [isSelectingCover, setIsSelectingCover] = useState(false);

  // Helper function to detect if drag is in the hand area
  const isInHandArea = (x: number, y: number) => {
    const handAreaTop = window.innerHeight - 150; // Hand area starts 200px from bottom
    return y >= handAreaTop;
  };

  // Helper function to detect which table card is under the cursor
  const getTableCardUnderCursor = (x: number, y: number) => {
    const elements = document.elementsFromPoint(x, y);
    const battleCardElement = elements.find(el => el.getAttribute('data-battle-index') !== null);
    if (battleCardElement) {
      const battleIndex = parseInt(battleCardElement.getAttribute('data-battle-index')!);
      return state.table_battles[battleIndex];
    }
    return null;
  };

  // Helper function to check if passing is possible
  const canPass = (draggedCard: Card) => {
    const table_battles = state.table_battles;
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

  // Helper function to determine what action should be taken
  const determineGameAction = (x: number, y: number, draggedCard: Card) => {
    if (isInHandArea(x, y)) {
      return { type: 'rearrange' };
    }

    const self_index = state.players.findIndex((player) => player.player_id === user_id);
    const isDefending = state.defender === self_index;

    if (isDefending) {
      const tableCardUnderCursor = getTableCardUnderCursor(x, y);
      const passIsPossible = canPass(draggedCard);

      if (tableCardUnderCursor && !tableCardUnderCursor.defense) {
        // Dragging to an uncovered attack card = cover
        return { type: 'cover', targetCard: tableCardUnderCursor.attack };
      } else if (!passIsPossible) {
        // Can't pass and in empty space - check if there's only one card to cover
        const uncoveredBattles = state.table_battles.filter(battle => !battle.defense);
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

  // Enhanced drag behavior with real-time rearrangement
  useEffect(() => {
    const handleMouseMove = (e: MouseEvent) => {
      if (isDraggingCard && draggedCardIndex !== null) {

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
          if (cardElement) {
            const targetIndex = parseInt(cardElement.getAttribute('data-card-index')!);
            if (targetIndex !== draggedCardIndex) {
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
          }
        }
      }
    };

    const handleTouchMove = (e: TouchEvent) => {
      if (isDraggingCard && draggedCardIndex !== null && e.touches.length > 0) {
        e.preventDefault();
        const touch = e.touches[0];

        // Update current cursor position
        setCurrentCursorPos({ x: touch.clientX, y: touch.clientY });

        // Check if we've moved far enough to consider this actual dragging
        if (dragStartPos && !isActuallyDragging) {
          const distance = Math.sqrt(
            Math.pow(touch.clientX - dragStartPos.x, 2) +
            Math.pow(touch.clientY - dragStartPos.y, 2)
          );
          if (distance > 10) { // 10 pixels threshold for actual dragging
            setIsActuallyDragging(true);
          }
        }

        // Check if dragging outside hand area for game actions
        if (isActuallyDragging && !isInHandArea(touch.clientX, touch.clientY)) {
          if (!isDraggingForGameAction) {
            setIsDraggingForGameAction(true);
            setDraggedCard(localHandOrder[draggedCardIndex]);
          }
        } else if (isDraggingForGameAction && isInHandArea(touch.clientX, touch.clientY)) {
          // Dragged back into hand area
          setIsDraggingForGameAction(false);
          setDraggedCard(null);
        }

        // Only allow swaps when actually dragging and still in hand area
        if (isActuallyDragging && !isDraggingForGameAction) {
          // Find what card we're hovering over for real-time swapping
          const elements = document.elementsFromPoint(touch.clientX, touch.clientY);
          const cardElement = elements.find(el => el.getAttribute('data-card-index') !== null);
          if (cardElement) {
            const targetIndex = parseInt(cardElement.getAttribute('data-card-index')!);
            if (targetIndex !== draggedCardIndex) {
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
          }
        }
      }
    };

    const handleMouseUp = (e: MouseEvent) => {
      if (isDraggingCard) {
        e.preventDefault();
        endCardDrag();
      }
    };

    const handleTouchEnd = (e: TouchEvent) => {
      if (isDraggingCard) {
        e.preventDefault();
        endCardDrag();
      }
    };

    if (isDraggingCard) {
      document.addEventListener('mousemove', handleMouseMove);
      document.addEventListener('touchmove', handleTouchMove, { passive: false });
      document.addEventListener('mouseup', handleMouseUp);
      document.addEventListener('touchend', handleTouchEnd);
    }

    return () => {
      document.removeEventListener('mousemove', handleMouseMove);
      document.removeEventListener('touchmove', handleTouchMove);
      document.removeEventListener('mouseup', handleMouseUp);
      document.removeEventListener('touchend', handleTouchEnd);
    };
  }, [isDraggingCard, draggedCardIndex, localHandOrder, hasSwapped, isActuallyDragging, touchStartTime, isDraggingForGameAction, draggedCard, currentCursorPos]);

  const scheduleCardRearrangeUpdate = (newOrder: Card[]) => {
    // Cancel existing timer
    if (rearrangeCardTimerRef.current) {
      clearTimeout(rearrangeCardTimerRef.current);
    }

    // Create indices array based on original order
    const originalHand = state?.self?.hand || [];
    const indices = newOrder.map(newCard =>
      originalHand.findIndex(origCard =>
        origCard.value === newCard.value && origCard.suit === newCard.suit
      )
    );

    // Set new 6-second timer
    rearrangeCardTimerRef.current = setTimeout(() => {
      rearrangeHand(state.id!, indices).catch(error => {
        console.error('Failed to rearrange hand:', error);
        // Revert to original order on error
        setLocalHandOrder(originalHand);
      });
    }, 5000);
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

      switch (action.type) {
        case 'attack':
          attack(cardsToUse).then(() => {
            console.log('Attack performed via drag');
            setSelectedCards([]); // Clear selection after successful action
          }).catch((e) => {
            console.error('Attack failed:', e.message);
          });
          break;

        case 'cover':
          if (action.targetCard) {
            // For cover, we can only cover one card at a time, so use just the first card
            // But we could extend this later to handle multiple covers
            const cardToUse = cardsToUse[0];
            cover([cardToUse], [action.targetCard]).then(() => {
              console.log('Cover performed via drag');
              setSelectedCards([]); // Clear selection after successful action
            }).catch((e) => {
              console.error('Cover failed:', e.message);
            });
          }
          break;

        case 'pass':
          pass(cardsToUse).then(() => {
            console.log('Pass performed via drag');
            setSelectedCards([]); // Clear selection after successful action
          }).catch((e) => {
            console.error('Pass failed:', e.message);
          });
          break;
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

  // Handle missing game data (GameView handles loading and error states)
  if (!state || !state.players || !state.players.length) {
    return <div>Loading...</div>;
  }

  const self_index = state.players.findIndex((player) => player.player_id === user_id);

  const isDefending = state.defender === self_index;


  return (
    <div style={{
      backgroundColor: '#982621',
      width: '100%',
      height: '100vh',
      touchAction: 'manipulation' // Allow taps and pinch-zoom but prevent double-tap zoom and panning
    }}>
      <CoverArrows state={state} coverMap={coverMap} />

      <div style={{ display: 'flex', flex: 1, justifyContent: 'center', alignItems: 'center' }}>
        <p style={{ color: 'white', fontSize: '24px', fontWeight: 'bold' }}>{state.name}</p>

        <DragShadow state={state} isDraggingForGameAction={isDraggingForGameAction} draggedCard={draggedCard} currentCursorPos={currentCursorPos} selectedCards={selectedCards} determineGameAction={determineGameAction} user_id={user_id} />

        <DeckAndFlipped state={state} />

        <ActionButtons state={state} isDefending={isDefending} pickup={pickup} setSelectedCards={setSelectedCards} good={good} setCoverMap={setCoverMap} isSelectingCover={isSelectingCover} coverMap={coverMap} selectedCards={selectedCards} self_index={self_index} isDraggingForGameAction={isDraggingForGameAction} draggedCardIndex={draggedCardIndex} startCardDrag={startCardDrag} isActuallyDragging={isActuallyDragging} cover={cover} attack={attack} localHandOrder={localHandOrder} pass={pass} setIsSelectingCover={setIsSelectingCover} />

        <div style={{ position: 'absolute', display: 'flex', flexDirection: 'column', top: 0, width: '100%', bottom: 0, alignItems: 'center', justifyContent: 'center' }}>
          <DefenderShield state={state} self_index={self_index} />

          <TableBattles state={state} coverMap={coverMap} self_index={self_index} isDraggingForGameAction={isDraggingForGameAction} isSelectingCover={isSelectingCover} setCoverMap={setCoverMap} selectedCards={selectedCards} />
        </div>


        {/* Player display section */}
        <PlayerRing state={state} self_index={self_index} playerCardPatternDataUrl={playerCardPatternDataUrl} tableBattlesLength={state.table_battles.length} />

      </div>
    </div>
  );
};