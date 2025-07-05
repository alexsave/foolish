import { Card, PersonalGame } from '../common/types';
import React, { useState, useEffect, useRef } from 'react';
import { useServer } from '../contexts/ServerContext';
import { useAuth } from '../contexts/AuthContext';

const SUIT_MAP: Record<number, string> = {
  // emojis
  0: '♠️',
  1: '♥️',
  2: '♣️',
  3: '♦️',
}

const VALUE_MAP: Record<number, string> = {
  1: '2',
  2: '3',
  3: '4',
  4: '5',
  5: '6',
  6: '7',
  7: '8',
  8: '9',
  9: '10',
  10: 'J',
  11: 'Q',
  12: 'K',
  13: 'A',
}

// Ok let's actually look at the game state to see if we are defending and modify options

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

    const self_index = state.players.findIndex((player) => player.id === user_id);
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

  const self_index = state.players.findIndex((player) => player.id === user_id);

  const isDefending = state.defender === self_index;

  // a set


  const CardDisplay = ({ card, onClick }: { card: Card, onClick?: () => void }) => {
    return (
      <div onClick={onClick} style={{ backgroundColor: 'white', width: '40px', height: '70px', borderRadius: '5px', border: '2px solid black', display: 'flex', justifyContent: 'center', alignItems: 'center' }}>
        <p style={{ margin: 0, fontSize: '18px', textAlign: 'center' }}>
          {VALUE_MAP[card.value]}
          <br />
          {SUIT_MAP[card.suit]}
        </p>
      </div>
    )
  }

  const generateCardBackPattern = (width: number, height: number): string => {
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const ctx = canvas.getContext('2d')!;

    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    // Red background color (#DC143C crimson red)
    const bgRed = 220;
    const bgGreen = 20;
    const bgBlue = 60;

    // Gold line color (#FFD700 gold)
    const lineRed = 255;
    const lineGreen = 215;
    const lineBlue = 0;

    // Scale grid size proportionally to card size (base size is 40x70)
    const baseWidth = 40;
    const scaleFactor = width / baseWidth;
    const gridSize = 8 * scaleFactor; // Scale the grid size
    const lineWidth = 0.5 * scaleFactor; // Scale line width too

    // Calculate angles for 30° and -30° diagonals
    const angle1 = Math.PI / 3; // 30 degrees
    const angle2 = -Math.PI / 3; // -30 degrees

    // Direction vectors for the diagonal lines
    const cos1 = Math.cos(angle1);
    const sin1 = Math.sin(angle1);
    const cos2 = Math.cos(angle2);
    const sin2 = Math.sin(angle2);

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const index = (y * width + x) * 4;

        // Calculate distance to diagonal lines using line equation
        // For 30° diagonal: distance from point to parallel lines spaced gridSize apart
        const dist1 = Math.abs((x * sin1 - y * cos1) % gridSize);
        const dist2 = Math.abs((x * sin2 - y * cos2) % gridSize);

        // Check if point is close enough to either diagonal line
        const onLine1 = Math.min(dist1, gridSize - dist1) < lineWidth;
        const onLine2 = Math.min(dist2, gridSize - dist2) < lineWidth;

        if (onLine1 || onLine2) {
          // Gold line
          data[index] = lineRed;
          data[index + 1] = lineGreen;
          data[index + 2] = lineBlue;
        } else {
          // Red background
          data[index] = bgRed;
          data[index + 1] = bgGreen;
          data[index + 2] = bgBlue;
        }
        data[index + 3] = 255; // Alpha
      }
    }

    ctx.putImageData(imageData, 0, 0);
    return canvas.toDataURL();
  };

  const CardBack = ({ deckSize = 36 }: { deckSize?: number }) => {
    const [patternDataUrl, setPatternDataUrl] = useState<string>('');

    useEffect(() => {
      // Generate the pattern once when component mounts
      const dataUrl = generateCardBackPattern(40, 70);
      setPatternDataUrl(dataUrl);
    }, []);

    return (
      <div style={{ position: 'relative', width: '40px', height: '70px' }}>
        {/* Multiple card layers to show deck thickness */}
        {Array.from({ length: Math.min(Math.ceil(deckSize / 6), 6) }).map((_, layerIndex) => (
          <div
            key={`deck-layer-${layerIndex}`}
            style={{
              position: 'absolute',
              top: `${-layerIndex * 2}px`,
              left: `${-layerIndex * 1}px`,
              width: '40px',
              height: '70px',
              backgroundColor: '#DC143C', // Fallback crimson red
              border: '1px solid #8B0000', // Dark red border
              borderRadius: '5px',
              zIndex: layerIndex,
              transform: 'rotate(90deg)',
              backgroundImage: patternDataUrl ? `url(${patternDataUrl})` : undefined,
              backgroundSize: '100% 100%',
              backgroundRepeat: 'no-repeat'
            }}
          />
        ))}

        {/* Top card with more detailed pattern */}
        <div style={{
          position: 'absolute',
          top: `${-Math.min(Math.ceil(deckSize / 6), 6) * 2}px`,
          left: `${-Math.min(Math.ceil(deckSize / 6), 6) * 1}px`,
          width: '40px',
          height: '70px',
          backgroundColor: '#DC143C', // Fallback crimson red
          border: '2px solid #8B0000',
          borderRadius: '5px',
          zIndex: 10,
          transform: 'rotate(90deg)',
          backgroundImage: patternDataUrl ? `url(${patternDataUrl})` : undefined,
          backgroundSize: '100% 100%',
          backgroundRepeat: 'no-repeat',
          boxShadow: '0 2px 4px rgba(0,0,0,0.3)'
        }} />
      </div>
    )
  }

  return (
    <div style={{
      backgroundColor: '#982621',
      width: '100%',
      height: '100vh',
      touchAction: 'manipulation' // Allow taps and pinch-zoom but prevent double-tap zoom and panning
    }}>

      {/* SVG overlay for arrows */}
      <svg style={{ position: 'absolute', top: 0, left: 0, width: '100%', height: '100%', pointerEvents: 'none', zIndex: 500 }}>
        {Array.from(coverMap.entries()).map(([coveringCard, coveredCard], index) => {
          // Skip if spectator (no self)
          if (!state.self) return null;

          // Find the position of the covering card (in hand)
          const handCardIndex = state.self.hand.findIndex(card =>
            card.value === coveringCard.value && card.suit === coveringCard.suit
          );

          // Find the position of the covered card (on table)
          const tableCardIndex = state.table_battles.findIndex(battle =>
            battle.attack.value === coveredCard.value && battle.attack.suit === coveredCard.suit
          );

          if (handCardIndex === -1 || tableCardIndex === -1) return null;

          // Calculate approximate positions
          // Hand cards are at the bottom center
          const handCardsStartX = window.innerWidth / 2 - (state.self.hand.length * 40) / 2;
          const handX = handCardsStartX + (handCardIndex * 40) + 20; // 20 is half card width
          const handY = window.innerHeight - 100; // approximate bottom position

          // Table cards are in the center
          const tableX = window.innerWidth / 2;
          const tableY = window.innerHeight / 2 + (tableCardIndex * 80) - (state.table_battles.length * 40); // spread them vertically

          return (
            <g key={`arrow-${index}`}>
              {/* Arrow line */}
              <line
                x1={handX}
                y1={handY}
                x2={tableX}
                y2={tableY}
                stroke="yellow"
                strokeWidth="3"
                markerEnd="url(#arrowhead)"
              />
            </g>
          );
        })}

        {/* Arrow marker definition */}
        <defs>
          <marker
            id="arrowhead"
            markerWidth="10"
            markerHeight="7"
            refX="9"
            refY="3.5"
            orient="auto"
          >
            <polygon
              points="0 0, 10 3.5, 0 7"
              fill="yellow"
            />
          </marker>
        </defs>
      </svg>

      <div style={{ display: 'flex', flex: 1, justifyContent: 'center', alignItems: 'center' }}>
        <p style={{ color: 'white', fontSize: '24px', fontWeight: 'bold' }}>{state.name}</p>

        {/* Floating action indicator during game action drag */}
        {isDraggingForGameAction && draggedCard && currentCursorPos && (() => {
          const action = determineGameAction(currentCursorPos.x, currentCursorPos.y, draggedCard);
          return action.type === 'attack' || action.type === 'cover' || action.type === 'pass';
        })() && (
            <div style={{
              position: 'absolute',
              left: currentCursorPos.x - 20,
              top: currentCursorPos.y + 10,
              backgroundColor: 'rgba(0, 0, 0, 0.8)',
              color: 'white',
              padding: '4px 8px',
              borderRadius: '4px',
              fontSize: '12px',
              zIndex: 1001,
              pointerEvents: 'none'
            }}>
              {(() => {
                const self_index = state.players.findIndex((player) => player.id === user_id);
                const isDefending = state.defender === self_index;
                const action = determineGameAction(currentCursorPos.x, currentCursorPos.y, draggedCard);

                // Check if the dragged card is part of selected cards
                const isDraggedCardSelected = selectedCards.some(selectedCard =>
                  selectedCard.value === draggedCard.value && selectedCard.suit === draggedCard.suit
                );

                // Use all selected cards if the dragged card is selected, otherwise just the dragged card
                const cardsToUse = isDraggedCardSelected && selectedCards.length > 0 ? selectedCards : [draggedCard];
                const cardCount = cardsToUse.length;
                const cardCountText = '';//cardCount > 1 ? ` (${cardCount})` : '';

                switch (action.type) {
                  case 'attack':
                    return `⚔️ Attack${cardCountText}`;
                  case 'cover':
                    return '🛡️ Cover';
                  case 'pass':
                    return `🔄 Pass${cardCountText}`;
                  default:
                    return '❓';
                }
              })()}
            </div>
          )}

        {/* Shadow cards showing what's being dragged */}
        {isDraggingForGameAction && draggedCard && currentCursorPos && (() => {
          const action = determineGameAction(currentCursorPos.x, currentCursorPos.y, draggedCard);
          return action.type === 'attack' || action.type === 'cover' || action.type === 'pass';
        })() && (
            <div style={{
              position: 'absolute',
              left: currentCursorPos.x - 10,
              top: currentCursorPos.y - 50,
              display: 'flex',
              flexDirection: 'row',
              alignItems: 'center',
              zIndex: 1002,
              pointerEvents: 'none'
            }}>
              {(() => {
                // Check if the dragged card is part of selected cards
                const isDraggedCardSelected = selectedCards.some(selectedCard =>
                  selectedCard.value === draggedCard.value && selectedCard.suit === draggedCard.suit
                );

                // Use all selected cards if the dragged card is selected, otherwise just the dragged card
                const cardsToUse = isDraggedCardSelected && selectedCards.length > 0 ? selectedCards : [draggedCard];

                return cardsToUse.map((card, index) => (
                  <div
                    key={`shadow-${card.value}-${card.suit}-${index}`}
                    style={{
                      border: '2px solid black',
                      backgroundColor: 'white',
                      width: '30px',
                      height: '50px',
                      borderRadius: '4px',
                      display: 'flex',
                      justifyContent: 'center',
                      alignItems: 'center',
                      marginRight: '2px',
                      boxShadow: '0 2px 4px rgba(0,0,0,0.3)',
                      opacity: 0.9
                    }}
                  >
                    <p style={{
                      pointerEvents: 'none',
                      userSelect: 'none',
                      textAlign: 'center',
                      fontSize: '12px',
                      margin: 0
                    }}>
                      {VALUE_MAP[card.value]}
                      <br />
                      {SUIT_MAP[card.suit]}
                    </p>
                  </div>
                ));
              })()}
            </div>
          )}

        <div style={{ display: 'flex', position: 'absolute', top: '0px', left: '0px', flexDirection: 'column', justifyContent: 'center', alignItems: 'center', height: '240px', width: '100px' }}>
          {state.deck_length > 0 && (
            <div style={{ position: 'relative' }}>
              <CardBack deckSize={state.deck_length} />
              <p style={{
                position: 'absolute',
                top: '50%',
                left: '50%',
                transform: 'translate(-50%, -50%)',
                margin: 0,
                color: 'white',
                fontSize: '16px',
                fontWeight: 'bold',
                textShadow: '1px 1px 2px rgba(0,0,0,0.8)',
                pointerEvents: 'none',
                zIndex: 1003
              }}>
                {state.deck_length + (state.flipped ? 1 : 0)}
              </p>
            </div>
          )}
          {state.flipped && (
            <div style={{ marginTop: state.deck_length > 0 ? '-30px' : '0px' }}>
              <CardDisplay card={state.flipped} />
            </div>
          )}
          {/* Trump indicator appears when deck and flipped card are gone */}
          {state.deck_length === 0 && !state.flipped && (
            <div style={{
              fontSize: '64px',
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center'
            }}>
              <span>{SUIT_MAP[state.power_suit]}</span>
            </div>
          )}
        </div>



        <div style={{ display: 'flex', flexDirection: 'column', position: 'absolute', bottom: '10px', left: '0px', right: '0px', justifyContent: 'end', alignItems: 'center', height: '200px' }}>
          {/* Always visible pickup and good buttons */}
          {state.self && (
            <div style={{
              position: 'absolute',
              bottom: '90px',
              right: '20px',
              display: 'flex',
              flexDirection: 'column',
              gap: '5px',
              zIndex: 999
            }}>
              {/* Pickup button for defenders */}
              {isDefending && state.table_battles.length > 0 && (
                <button
                  style={{
                    width: '60px',
                    height: '40px',
                    fontSize: '12px',
                    backgroundColor: '#ff6b6b',
                    color: 'white',
                    border: 'none',
                    borderRadius: '4px',
                    cursor: 'pointer'
                  }}
                  onClick={() => {
                    pickup().then(() => {
                      setSelectedCards([]);
                    }).catch((e) => {
                      console.error(e.message);
                    })
                  }}
                >
                  Pickup
                </button>
              )}

              {/* Good button for attackers when all attacks are covered */}
              {!isDefending && state.table_battles.length > 0 && state.table_battles.every(battle => battle.defense) && (
                <button
                  style={{
                    width: '60px',
                    height: '40px',
                    fontSize: '12px',
                    backgroundColor: '#51cf66',
                    color: 'white',
                    border: 'none',
                    borderRadius: '4px',
                    cursor: 'pointer'
                  }}
                  onClick={() => {
                    good().then(() => {
                      setSelectedCards([]);
                    }).catch((e) => {
                      console.error(e.message);
                    })
                  }}
                >
                  Good
                </button>
              )}
            </div>
          )}

          {
            state.self && selectedCards.length > 0 && <div style={{ backgroundColor: 'rgba(0, 0, 0, 0.5)', zIndex: 999, height: '50px ' }}>

              {
                isDefending ? (
                  <>
                    {/* During first attack, defender can do nothing */}
                    {state.table_battles.length > 0 && (
                      <>
                        {/* Pass is only shown if no attack card is covered */}
                        {state.table_battles.every(battle => !battle.defense) && (
                          <button
                            style={{ width: '60px', height: '50px' }}
                            onClick={() => {
                              pass(selectedCards).then(() => {
                                setSelectedCards([]);
                              }).catch((e) => {
                                console.error(e.message);
                              })
                            }}
                          >
                            Pass
                          </button>
                        )}

                        {/* Cover is only shown if there are uncovered cards */}
                        {state.table_battles.some(battle => !battle.defense) && (
                          <button style={{ width: '60px', height: '50px' }} onClick={() => {
                            // If there's exactly 1 uncovered card, cover it immediately
                            const uncoveredBattles = state.table_battles.filter(battle => !battle.defense);
                            if (uncoveredBattles.length === 1) {
                              // Auto-cover the single uncovered card
                              const attackCard = uncoveredBattles[0].attack;
                              const coverCard = selectedCards[0];
                              setCoverMap(new Map().set(coverCard, attackCard));

                              // Immediately execute the cover
                              cover([coverCard], [attackCard]).then(() => {
                                setSelectedCards([]);
                                setCoverMap(new Map());
                              }).catch((e) => {
                                console.error(e.message);
                              });
                            } else {
                              // Multiple uncovered cards, need to select which one to cover
                              setIsSelectingCover(true);
                            }
                          }}>Cover</button>
                        )}

                        {/* Actually Cover is shown when in cover selection mode OR when there are covers queued */}
                        {(isSelectingCover || coverMap.size > 0) && (
                          <>
                            <button style={{ width: '60px', height: '50px' }} onClick={() => {
                              const coverCards = Array.from(coverMap.keys());
                              const attackCards = Array.from(coverMap.values());
                              cover(coverCards, attackCards).then(() => {
                                setSelectedCards([]);
                                setCoverMap(new Map());
                              }).catch((e) => {
                                console.error(e.message);
                              })
                              setIsSelectingCover(false);
                            }}>Actually Cover</button>

                            {/* Cancel Cover button to reset cover selection */}
                            <button style={{ width: '60px', height: '50px' }} onClick={() => {
                              setIsSelectingCover(false);
                              setCoverMap(new Map());
                            }}>Cancel Cover</button>
                          </>
                        )}
                      </>
                    )}
                  </>
                ) : (
                  <>
                    {/* Attack is only shown when valid */}
                    {(state.table_battles.length > 0 || self_index === state.first_attacker) && (
                      <button
                        style={{ width: '60px', height: '50px' }}
                        onClick={() => attack(selectedCards).then(() => {
                          setSelectedCards([]);
                        }).catch((e) => {
                          console.error(e.message);
                        })}
                      >
                        Attack
                      </button>
                    )}
                  </>
                )
              }

            </div>
          }
          <div style={{
            display: 'flex',
            flexDirection: 'row',
            alignItems: 'center',
            justifyContent: 'center',
            width: '100%'
          }}>

            {
              state.self ? localHandOrder.map((card, index) => {
                const isSelected = selectedCards.some(selectedCard =>
                  selectedCard.value === card.value && selectedCard.suit === card.suit
                );
                const isDragging = isActuallyDragging && draggedCardIndex === index;
                const isDraggingForAction = isDraggingForGameAction && draggedCardIndex === index;

                // Determine the style based on state
                let cardStyle: React.CSSProperties;
                if (isDraggingForAction) {
                  // Special styling for game action drag
                  //hmm
                  /*cardStyle = {
                    border: '3px solid yellow',
                    backgroundColor: 'lightyellow',
                    boxShadow: '0 4px 8px rgba(0,0,0,0.3)'
                  };*/
                  cardStyle = {
                    border: '2px solid red',
                    backgroundColor: 'white'
                  };
                } else if (isSelected) {
                  cardStyle = {
                    border: '2px solid red',
                    backgroundColor: 'white'
                  };
                } else {
                  cardStyle = {
                    border: '2px solid black',
                    backgroundColor: 'white'
                  };
                }

                return (
                  <div
                    key={'' + card.value + card.suit}
                    data-card-index={index}
                    onMouseDown={(e) => startCardDrag(e, index)}
                    onTouchStart={(e) => startCardDrag(e, index)}
                    style={{
                      ...cardStyle,
                      flex: '1 1 0',
                      minWidth: '20px',
                      maxWidth: '40px',
                      zIndex: 1000,
                      height: '70px',
                      borderRadius: '5px',
                      display: 'flex',
                      justifyContent: 'center',
                      alignItems: 'center',
                      opacity: (isDragging && !isDraggingForAction) ? 0.3 : 1, // Only fade for rearranging, not game actions
                      transition: 'all 0.1s ease',
                      cursor: 'move',
                      userSelect: 'none',
                      margin: '0 1px'
                    }}

                  >
                    <p style={{
                      pointerEvents: 'none', // Prevent text selection
                      userSelect: 'none',
                      textAlign: 'center',
                      fontSize: '20px'
                    }}>
                      {VALUE_MAP[card.value]}
                      <br />
                      {SUIT_MAP[card.suit]}
                    </p>
                  </div>
                )
              }) : <p style={{ color: 'white', fontSize: '18px' }}>Spectating</p>
            }
          </div>

        </div>

        <div style={{ position: 'absolute', display: 'flex', flexDirection: 'column', top: 0, width: '100%', bottom: 0, alignItems: 'center', justifyContent: 'center' }}>


          {/* Debug: Green arrow from shield center towards defender player (20px long) */}
          <svg style={{ position: 'absolute', top: 0, left: 0, width: '100%', height: '100%', pointerEvents: 'none', zIndex: 50 }}>
            {(() => {
              // Only show for defender
              const defenderPlayer = state.players[state.defender];
              if (!defenderPlayer) return null;

              const visual_index = (state.defender - self_index + state.players.length) % state.players.length;
              const radians = (2) * Math.PI * visual_index / (state.players.length);

              // Calculate shield position (same as debug dots)

              const H = window.innerHeight;
              const W = window.innerWidth;
              const aPct = 35;                          // ellipse semi-axis in %
              const cxPct = 50, cyPct = 50;             // centre in %

              // defender in %
              const dxPct = aPct * Math.cos(radians + Math.PI / 2);  // –sinθ
              const dyPct = aPct * Math.sin(radians + Math.PI / 2);  //  cosθ

              // length of that vector in px
              const dxPx = dxPct * W / 100;
              const dyPx = dyPct * H / 100;
              const rPx = Math.hypot(dxPx, dyPx);

              // step 60 px inward  (= keep direction, shorten length)
              const startScale = (rPx - 36) / rPx;
              const arrowStartX = cxPct + dxPct * startScale;
              const arrowStartY = cyPct + dyPct * startScale;

              const endScale = (rPx - 35) / rPx;
              const arrowEndX = cxPct + dxPct * endScale;
              const arrowEndY = cyPct + dyPct * endScale;

              return (
                <line
                  key="debug-arrow-defender"
                  x1={`${arrowStartX}%`}
                  y1={`${arrowStartY}%`}
                  x2={`${arrowEndX}%`}
                  y2={`${arrowEndY}%`}
                  stroke="black"
                  strokeWidth="4"
                  markerEnd="url(#blackArrowHead)"
                />
              );
            })()}

            {/* Arrow marker definition for black arrows */}
            <defs>
              <marker
                id="blackArrowHead"
                markerWidth="4"
                markerHeight="4"
                refX="1"
                refY="2"
                orient="auto"
              >
                <polygon
                  points="0 0, 2 2, 0 4"
                  fill="black"
                />
              </marker>
            </defs>
          </svg>

          {/* Shield and arrow pointing to defender */}
          {(() => {
            const defenderPlayer = state.players[state.defender];
            if (!defenderPlayer) return null;

            const visual_index = (state.defender - self_index + state.players.length) % state.players.length;

            // Calculate defender position
            const H = window.innerHeight;
            const W = window.innerWidth;
            const aPct = 35;                          // ellipse semi-axis in %
            const cxPct = 50, cyPct = 50;             // centre in %

            const radians = 2 * Math.PI * visual_index / state.players.length;

            // defender in %
            const dxPct = aPct * Math.cos(radians + Math.PI / 2);  // –sinθ
            const dyPct = aPct * Math.sin(radians + Math.PI / 2);  //  cosθ

            // length of that vector in px
            const dxPx = dxPct * W / 100;
            const dyPx = dyPct * H / 100;
            const rPx = Math.hypot(dxPx, dyPx);

            // step 60 px inward  (= keep direction, shorten length)
            const scale = (rPx - 55) / rPx;
            const shieldXPct = cxPct + dxPct * scale;
            const shieldYPct = cyPct + dyPct * scale;

            return (
              <div style={{
                position: 'absolute',
                left: `${shieldXPct}%`,
                top: `${shieldYPct}%`,
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center'
              }}>


                {/* Shield */}
                <div style={{
                  position: 'absolute',
                  fontSize: '24px',
                  zIndex: 400
                }}>
                  🛡️
                </div>
              </div>
            );
          })()}

          {
            state.table_battles.map((battle, index) => {
              let containerStyle: React.CSSProperties = {
                border: '1px solid black',
                display: 'flex',
                flexDirection: 'row',
                alignItems: 'center',
                justifyContent: 'center'
              };

              if (coverMap.values().some(c => c.value === battle.attack.value && c.suit === battle.attack.suit)) {
                containerStyle.border = '3px solid red';
              }

              // Add highlighting for valid drop zones during game action drag
              const isDefending = state.defender === self_index;
              const isValidCoverTarget = isDraggingForGameAction && isDefending && !battle.defense;

              if (isValidCoverTarget) {
                containerStyle.border = '3px solid green';
                containerStyle.backgroundColor = 'rgba(0, 255, 0, 0.1)';
              }

              return <div key={battle.attack.value + ' ' + battle.attack.suit} style={containerStyle}>
                <div data-battle-index={index}>
                  <CardDisplay
                    card={battle.attack}
                    onClick={() => {
                      if (isSelectingCover) {
                        setCoverMap(new Map(coverMap.set(selectedCards[0], battle.attack)));
                        console.log(coverMap);
                        // Don't set isSelectingCover to false here - keep it true so "Actually Cover" button remains visible
                      }
                    }}
                  />
                </div>
                {battle.defense && <CardDisplay card={battle.defense} />}
              </div>
            })
          }
        </div>

        {/* Player display section */}
        {
          state.players.map((player, index) => {

            const visual_index = (index - self_index + state.players.length) % state.players.length;
            const radians = (2) * Math.PI * visual_index / (state.players.length);
            const x = ((-1 * Math.sin(radians) * 35) + 50) + '%';
            const y = ((Math.cos(radians) * 35) + 50) + '%';

            return <div key={player.id} style={{
              position: 'absolute',
              top: y,
              left: x,
              display: 'flex',
              flexDirection: 'column',
              alignItems: 'center',
              width: '80px',
              height: '80px',
              transform: 'translate(-50%, -50%)' // Center the element relative to its position
            }}>


              {/* Sword area (top) - either sword or empty space */}
              {index === state.first_attacker && state.table_battles.length === 0 ? (
                <div style={{
                  fontSize: '16px',
                  height: '20px',
                  display: 'flex',
                  alignItems: 'center',
                  justifyContent: 'center'
                }}>
                  ⚔️
                </div>
              ) : (
                <div style={{ height: '20px' }} />
              )}

              {/* Player name (center) */}
              <p style={{
                margin: 0,
                fontSize: '12px',
                color: 'white',
                textAlign: 'center',
                height: '20px',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center'
              }}>
                {player.name}
              </p>

              {/* Cards area (bottom) */}
              {player.hand_length && player.hand_length > 0 ? (
                <div style={{
                  display: 'flex',
                  flexDirection: 'row',
                  alignItems: 'center',
                  justifyContent: 'center',
                  position: 'relative',
                  height: '20px',
                  width: '100px'
                }}>
                  {Array.from({ length: Math.min(player.hand_length, 8) }).map((_, cardIndex) => {

                    // Calculate proper centering: total span divided by 2, then offset each card

                    const mid = (player.hand_length + 1) / 2;
                    const halfCardWidth = 12 / 2;
                    const halfDivWidth = 100 / 2;

                    return (
                      <div
                        key={`player-${player.id}-card-${cardIndex}`}
                        style={{
                          backgroundColor: '#DC143C', // Fallback red background
                          width: '12px',
                          height: '18px',
                          borderRadius: '2px',
                          border: '1px solid #8B0000', // Same dark red border
                          position: 'absolute',
                          left: `${halfDivWidth + (cardIndex - mid) * 2 - halfCardWidth}px`,
                          zIndex: cardIndex,
                          boxShadow: '0 1px 2px rgba(0,0,0,0.3)',
                          backgroundImage: playerCardPatternDataUrl ? `url(${playerCardPatternDataUrl})` : undefined,
                          backgroundSize: '100% 100%',
                          backgroundRepeat: 'no-repeat'
                        }}
                      />
                    );
                  })}

                  {/* Card count overlay */}
                  <div style={{
                    position: 'absolute',
                    top: '50%',
                    left: '50%',
                    transform: 'translate(-50%, -50%)',
                    color: 'white',
                    fontSize: '10px',
                    fontWeight: 'bold',
                    zIndex: 15,
                    pointerEvents: 'none',
                    textShadow: '1px 1px 2px rgba(0,0,0,0.8)'
                  }}>
                    {player.hand_length}
                  </div>
                </div>
              ) : (
                <div style={{ height: '20px' }} />
              )}
            </div>
          })
        }


      </div>
    </div>
  );
};