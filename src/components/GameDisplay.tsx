import { Card, PersonalGame } from '../common/types';
import React, { useState, useEffect, useRef } from 'react';
import { useServer } from '../contexts/ServerContext';
import { useParams } from 'react-router-dom';
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
  const { game, attack, game_id, pass, pickup, setGameIdFromUrl, loadGame, cover, good, rearrangeHand } = useServer();
  const urlGameId = useParams().game_id?.toLowerCase() || null;
  const state = game as PersonalGame;
  const [selectedCards, setSelectedCards] = useState<Card[]>([]);

  const [coverMap, setCoverMap] = useState<Map<Card, Card>>(new Map());

  // Enhanced drag and drop state for cards
  const [draggedCardIndex, setDraggedCardIndex] = useState<number | null>(null);
  const [isDraggingCard, setIsDraggingCard] = useState(false);
  const [draggedCardElement, setDraggedCardElement] = useState<HTMLElement | null>(null);
  const [localHandOrder, setLocalHandOrder] = useState<Card[]>([]);
  const rearrangeCardTimerRef = useRef<NodeJS.Timeout | null>(null);

  useEffect(() => {
    if (urlGameId && urlGameId !== game_id) {
      setGameIdFromUrl(urlGameId);
      //loadGame(urlGameId);
    }
  }, [urlGameId, game_id, setGameIdFromUrl, loadGame]);

  // Update local hand order when game changes
  useEffect(() => {
    if (state?.self?.hand) {
      setLocalHandOrder(state.self.hand);
    }
  }, [state?.self?.hand]);

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

  // Enhanced drag behavior with mouse following for cards
  useEffect(() => {
    const handleMouseMove = (e: MouseEvent) => {
      if (isDraggingCard && draggedCardElement) {
        const style = draggedCardElement.style;
        style.position = 'fixed';
        style.top = e.clientY - draggedCardElement.clientHeight / 2 + 'px';
        style.left = e.clientX - draggedCardElement.clientWidth / 2 + 'px';
        style.zIndex = '1500';
        style.pointerEvents = 'none';
      }
    };

    const handleGlobalDragEnd = (e: DragEvent) => {
      if (isDraggingCard) {
        setIsDraggingCard(false);
        setDraggedCardIndex(null);
        setDraggedCardElement(null);
      }
    };

    const handleMouseUp = (e: MouseEvent) => {
      if (isDraggingCard) {
        setIsDraggingCard(false);
        setDraggedCardIndex(null);
        setDraggedCardElement(null);
      }
    };

    const handleTouchEnd = (e: TouchEvent) => {
      if (isDraggingCard) {
        setIsDraggingCard(false);
        setDraggedCardIndex(null);
        setDraggedCardElement(null);
      }
    };

    if (isDraggingCard && draggedCardElement) {
      document.addEventListener('mousemove', handleMouseMove);
      document.addEventListener('dragend', handleGlobalDragEnd);
      document.addEventListener('mouseup', handleMouseUp);
      document.addEventListener('touchend', handleTouchEnd);
    } else if (draggedCardElement) {
      // Clean up styles when drag ends
      const style = draggedCardElement.style;
      style.position = '';
      style.top = '';
      style.left = '';
      style.zIndex = '';
      style.pointerEvents = '';
    }

    return () => {
      document.removeEventListener('mousemove', handleMouseMove);
      document.removeEventListener('dragend', handleGlobalDragEnd);
      document.removeEventListener('mouseup', handleMouseUp);
      document.removeEventListener('touchend', handleTouchEnd);
    };
  }, [isDraggingCard, draggedCardElement]);

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
      rearrangeHand(game_id!, indices).catch(error => {
        console.error('Failed to rearrange hand:', error);
        // Revert to original order on error
        setLocalHandOrder(originalHand);
      });
    }, 6000);
  };

  const handleCardDragStart = (e: React.DragEvent, index: number) => {
    
    // Cancel any pending rearrange update since user is still actively dragging
    if (rearrangeCardTimerRef.current) {
      clearTimeout(rearrangeCardTimerRef.current);
      rearrangeCardTimerRef.current = null;
    }

    e.dataTransfer.setData('text/plain', '');
    setIsDraggingCard(true);
    setDraggedCardIndex(index);
    setDraggedCardElement(e.currentTarget as HTMLElement);
    e.dataTransfer.effectAllowed = 'move';
    
  };

  const handleCardDragEnd = (e: React.DragEvent) => {
    
    if (!isDraggingCard || draggedCardIndex === null) {
      return;
    }
    
    setIsDraggingCard(false);
    
    // Reset drag state
    setDraggedCardIndex(null);
    setDraggedCardElement(null);
    
    // Reset element styles
    const element = e.currentTarget as HTMLElement;
    element.style.position = '';
    element.style.top = '';
    element.style.left = '';
    element.style.zIndex = '';
    
  };

  const handleCardDragOver = (e: React.DragEvent, index: number) => {
    if (!isDraggingCard || draggedCardIndex === null) return;

    e.preventDefault();
    e.dataTransfer.dropEffect = 'move';

    if (index !== draggedCardIndex) {
      const newOrder = [...localHandOrder];
      const draggedCard = newOrder[draggedCardIndex];

      // Remove dragged card from current position
      newOrder.splice(draggedCardIndex, 1);

      // Insert at new position
      newOrder.splice(index, 0, draggedCard);

      setLocalHandOrder(newOrder);
      setDraggedCardIndex(index);
      scheduleCardRearrangeUpdate(newOrder);
    }
  };

  const handleCardDrop = (e: React.DragEvent) => {
    e.preventDefault();
    // Actual drop logic handled in handleCardDragEnd
  };

  if (!state || !state.players || !state.players.length) {
    return <div>Loading...</div>;
  }

  const self_index = state.players.findIndex((player) => player.id === user_id);

  const isDefending = state.currently_attacked === self_index;

  // a set


  const CardDisplay = ({ card, onClick }: { card: Card, onClick?: () => void }) => {
    return (
      <div onClick={onClick} style={{ backgroundColor: 'white', width: '40px', height: '70px', borderRadius: '5px', border: '1px solid black', display: 'flex', justifyContent: 'center', alignItems: 'center' }}>
        <p>{VALUE_MAP[card.value] + SUIT_MAP[card.suit]}</p>
      </div>
    )
  }

  const CardBack = () => {
    return (
      <div style={{ backgroundColor: 'black', width: '40px', height: '70px', borderRadius: '5px', border: '1px solid black', display: 'flex', justifyContent: 'center', alignItems: 'center' }}>
        <p>?</p>
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
        <p>FOOLISH</p>
        <div style={{ display: 'flex', position: 'absolute', top: '0px', left: '0px', flexDirection: 'column', justifyContent: 'center', alignItems: 'center', height: '300px', width: '100px' }}>
          {state.flipped && <CardDisplay card={state.flipped} />}
          <CardBack />
          <p>{state.deck_length}</p>
        </div>
        <div style={{ display: 'flex', flexDirection: 'column', position: 'absolute', bottom: '10px', left: '0px', right: '0px', justifyContent: 'end', alignItems: 'center', height: '200px' }}>
          {
            state.self && selectedCards.length > 0 && <div style={{ backgroundColor: 'rgba(0, 0, 0, 0.5)', zIndex: 999, height: '50px '}}>

              {
                isDefending ? (
                  <>
                    <button style={{ width: '60px', height: '50px' }} onClick={() => {
                      pass(selectedCards).then(() => {
                        setSelectedCards([]);
                      }).catch((e) => {
                        console.error(e.message);
                      })
                    }}>Pass</button>
                    <button style={{ width: '60px', height: '50px' }} onClick={() => {
                      pickup().then(() => {
                        // add cards to hand???
                        setSelectedCards([]);
                      }).catch((e) => {
                        console.error(e.message);
                      })
                    }}>Pickup</button>

                    <button style={{ width: '60px', height: '50px' }} onClick={() => {
                      setIsSelectingCover(true);
                    }}>Cover</button>

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
                  </>
                ) : (
                  <>
                  <button style={{ width: '60px', height: '50px' }} onClick={() => attack(selectedCards).then(() => {
                    setSelectedCards([]);
                  }).catch((e) => {
                    console.error(e.message);
                  })}>Attack</button>
                  <button style={{ width: '60px', height: '50px' }} onClick={() => good().then(() => {
                    setSelectedCards([]);
                  }).catch((e) => {
                    console.error(e.message);
                  })}>Good</button>
                  </>
                )
              }

            </div>
          }
          <div style={{ display: 'flex', flexDirection: 'row', alignItems: 'center', justifyContent: 'center' }}>

            {
              state.self ? localHandOrder.map((card, index) => {
                const isSelected = selectedCards.some(selectedCard => 
                  selectedCard.value === card.value && selectedCard.suit === card.suit
                );
                const isDragging = isDraggingCard && draggedCardIndex === index;
                const style = isSelected ? { border: '3px solid red' } : { border: '1px solid black' };
                
                return (
                  <div
                    key={'' + card.value + card.suit}
                    draggable={true}
                    onDragStart={(e) => handleCardDragStart(e, index)}
                    onDragOver={(e) => handleCardDragOver(e, index)}
                    onDrop={handleCardDrop}
                    onDragEnd={handleCardDragEnd}
                    style={{ 
                      ...style, 
                      zIndex: isDragging ? 1500 : 1000, 
                      backgroundColor: 'white', 
                      width: '40px', 
                      height: '70px', 
                      borderRadius: '5px', 
                      display: 'flex', 
                      justifyContent: 'center', 
                      alignItems: 'center',
                      opacity: isDragging ? 0.3 : 1,
                      transition: isDragging ? 'none' : 'all 0.2s ease',
                      transform: isDragging ? 'scale(1.05)' : 'scale(1)',
                      position: 'relative',
                      pointerEvents: isDragging ? 'none' : 'auto',
                      cursor: 'move'
                    }}
                    onClick={(e) => {
                      e.stopPropagation();
                      if (isSelected) {
                        setSelectedCards(selectedCards.filter(c => !(c.value === card.value && c.suit === card.suit)));
                      } else {
                        setSelectedCards([...selectedCards, card]);
                      }
                    }}
                  >
                    {/* Invisible overlay to prevent text selection during drag operations */}
                    <div style={{
                      position: 'absolute',
                      top: 0,
                      left: 0,
                      right: 0,
                      bottom: 0,
                      zIndex: 1600,
                      pointerEvents: isDraggingCard ? 'auto' : 'none',
                      userSelect: 'none'
                    }} />
                    <p style={{ zIndex: 10 }}>{VALUE_MAP[card.value] + SUIT_MAP[card.suit]}</p>
                  </div>
                )
              }) : <p style={{ color: 'white', fontSize: '18px' }}>Spectating</p>
            }
          </div>

        </div>

        <div style={{ position: 'absolute', display: 'flex', flexDirection: 'column', top: 0, width: '100%', bottom: 0, alignItems: 'center', justifyContent: 'center' }}>
          {
            state.table_battles.map((battle, index) => {
              let border = { border: '1px solid black' };
              if (coverMap.values().some(c => c.value === battle.attack.value && c.suit === battle.attack.suit)) {
                border = { border: '3px solid red' };
              }
              return <div key={battle.attack.value + ' ' + battle.attack.suit} style={{ ...border, display: 'flex', flexDirection: 'row', alignItems: 'center', justifyContent: 'center' }}>
                <CardDisplay
                  card={battle.attack}
                  onClick={() => {
                    if (isSelectingCover) {
                      setCoverMap(new Map(coverMap.set(selectedCards[0], battle.attack)));
                      console.log(coverMap);
                      setIsSelectingCover(false);
                    }
                  }}
                />
                {battle.defense && <CardDisplay card={battle.defense} />}
              </div>
            })
          }
          {
            state.players.map((player, index) => {

              const visual_index = (index - self_index + state.players.length) % state.players.length;
              // array of 100 black squares
              //Array.from({length: state.players.length-1}).map((_, index) => {
              const radians = (2) * Math.PI * visual_index / (state.players.length)// + Math.PI / 4;
              const x = ((-1 * Math.sin(radians) * 30) + 50) + '%';
              const y = ((Math.cos(radians) * 30) + 50) + '%';

              let color = 'black';
              if (index === state.currently_attacked) {
                color = 'red';
              } else if (index === state.first_attacker) {
                color = 'orange';
              }

              return <div key={player.id} style={{ backgroundColor: color, height: '10px', width: '10px', position: 'absolute', top: y, left: x }}>
                <p>{player.name}</p>
                {player.hand_length && <p>{player.hand_length}</p>}
              </div>
            })
          }
        </div>
      </div>
    </div>
  );
};