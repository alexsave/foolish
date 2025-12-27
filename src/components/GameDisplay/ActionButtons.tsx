import { Card, PersonalGame } from "../../common/types";
import { useAuth } from "../../contexts/AuthContext";
import { useServer } from "../../contexts/ServerContext";
import { useAnimation } from "../../contexts/AnimationContext";
import { useGame } from "../../contexts/GameContext";
import { useDrag } from "../../contexts/DragContext";
import { CardFace} from "./CardFace";
import { useWoodStyle } from "../WoodTexture";
import { useState, useEffect, useRef } from "react";

const CardDiv = ({ user_id }: { user_id: string }) => {
    const { game, localHandOrder } = useServer() as { game: PersonalGame, localHandOrder: Card[] };

    const { selectedCards } = useGame();

    const { draggedCardIndex, isDraggingForGameAction, startCardDrag, isActuallyDragging } = useDrag();

    if (!game || !game.self) {
        return <p style={{ color: 'white', fontSize: '18px' }}>Spectating</p>
    }

    return <div 
        data-touch-interactive
        style={{
            display: 'flex',
            flexDirection: 'row',
            alignItems: 'center',
            justifyContent: 'center',
            width: '100%'
        }}> {localHandOrder.map((card, index) => {
        const isSelected = selectedCards.some(selectedCard =>
            selectedCard.value === card.value && selectedCard.suit === card.suit
        );
        const isDragging = isActuallyDragging && draggedCardIndex === index;
        const isDraggingForAction = isDraggingForGameAction && draggedCardIndex === index;

        // Determine the border color based on state
        let borderColor = 'black';
        if (isDraggingForAction || isSelected) {
            borderColor = 'red';
        }

        return <CardFace
            card={card}
            playerId={user_id}
            key={'' + card.value + card.suit}
            data-card-index={index}
            data-location="hand"
            data-player-id={user_id}
            data-card={`${card.suit}-${card.value}`}
            draggable={true}
            onMouseDown={(e) => startCardDrag(e, index)}
            onTouchStart={(e) => startCardDrag(e, index)}
            onClick={() => /*bruh*/true}
            style={{
                flex: '1 1 0',
                minWidth: '20px',
                maxWidth: '50px',
                zIndex: 1000,
                height: '70px',
                borderRadius: '5px',
                display: 'flex',
                justifyContent: 'center',
                alignItems: 'center',
                opacity: (isDragging && !isDraggingForAction) ? 0.3 : 1,
                transition: 'all 0.1s ease',
                cursor: 'move',
                userSelect: 'none',
                margin: '0 1px',
                border: `2px solid ${borderColor}`,
                backgroundColor: 'white'
            }}
        />

    })} </div>
}

export const ActionButtons = () => {
    const { user_id } = useAuth();
    const { game } = useServer() as { game: PersonalGame };
    const { pickup, good } = useAnimation();

    
    // Track if "good" button was just clicked to hide it immediately
    const [goodButtonClicked, setGoodButtonClicked] = useState(false);
    
    // Track previous state of shouldShowGoodButton to detect transitions
    const prevShouldShowGoodButtonRef = useRef(false);
    
    // Use wood texture hooks at the top level
    const woodStylePickup = useWoodStyle(0.15);
    const woodStyleGood = useWoodStyle(0.85);

    // Calculate values needed for the effect (safe even if game is null)
    const self_index = game?.players.findIndex((player) => player.player_id === user_id) ?? -1;
    const isDefending = game && self_index !== -1 ? game.defender === self_index : false;
    const canPlay = game?.self?.hand.some(card => 
        game?.table_battles.some(battle => battle.attack.value === card.value || battle.defense?.value === card.value)
    ) ?? false;
    const shouldShowGoodButton = !isDefending && 
        (game?.table_battles.length ?? 0) > 0 && 
        (game?.table_battles.every(battle => battle.defense) ?? false) && 
        canPlay;

    // Reset the clicked flag only when transitioning from NOT possible to possible
    useEffect(() => {
        const prevShouldShow = prevShouldShowGoodButtonRef.current;
        
        // If it wasn't possible before but is now possible, reset the flag
        if (!prevShouldShow && shouldShowGoodButton && goodButtonClicked) {
            setGoodButtonClicked(false);
        }
        
        // Update the ref for next render
        prevShouldShowGoodButtonRef.current = shouldShowGoodButton;
    }, [shouldShowGoodButton, goodButtonClicked]);

    // Handle case where game is not loaded yet
    if (!game || !game.self) {
        return <div>
        </div>
    }


    const isOut = game.self.status === 'out';
    if (isOut) {
        return <div>
        </div>
    }


    return <div 
        data-touch-interactive
        style={{ display: 'flex', flexDirection: 'column', position: 'absolute', bottom: '10px', left: '0px', right: '0px', justifyContent: 'end', alignItems: 'center', height: '200px' }}>
        {/* Always visible pickup and good buttons */}
        {game && game.self && <div 
            data-touch-interactive
            style={{
                position: 'absolute',
                bottom: '90px',
                right: '20px',
                display: 'flex',
                flexDirection: 'column',
                gap: '5px',
                zIndex: 999
            }}>
            {/* Pickup button for defenders */}
            {isDefending && game.table_battles.length > 0 && <button
                style={{
                    ...woodStylePickup, // Wood texture for pickup button
                    width: '60px',
                    height: '40px',
                    fontSize: '12px',
                    fontWeight: 'bold',
                    border: '2px solid #5D3A1A',
                    borderRadius: '0',
                    cursor: 'pointer',
                    position: 'relative' as const,
                    overflow: 'hidden' as const,
                    boxShadow: `
                        inset 0 1px 0 rgba(255,255,255,0.2),
                        inset 0 -1px 0 rgba(0,0,0,0.3),
                        0 2px 4px rgba(0,0,0,0.4)`,
                    transition: 'all 0.2s ease',
                    mixBlendMode: 'normal' as const,
                }}
                onMouseEnter={(e) => {
                    e.currentTarget.style.filter = 'brightness(1.1) contrast(1.1)';
                    e.currentTarget.style.transform = 'translateY(-1px)';
                }}
                onMouseLeave={(e) => {
                    e.currentTarget.style.filter = '';
                    e.currentTarget.style.transform = '';
                }}
                onClick={() => pickup().then(() => {
                }).catch((e) => {
                    console.error(e.message);
                })}
            >
                <span style={{
                    color: 'rgba(70, 35, 20, 0.8)',
                    mixBlendMode: 'color-burn',
                    filter: 'contrast(1.2) brightness(0.9)',
                }}>Pickup</span>
            </button>
            }

            {/* Good button for attackers when all attacks are covered */}
            {shouldShowGoodButton && !goodButtonClicked && <button
                style={{
                    ...woodStyleGood, // Wood texture for good button
                    width: '60px',
                    height: '40px',
                    fontSize: '12px',
                    fontWeight: 'bold',
                    border: '2px solid #5D3A1A',
                    borderRadius: '0',
                    cursor: 'pointer',
                    position: 'relative' as const,
                    overflow: 'hidden' as const,
                    boxShadow: `
                        inset 0 1px 0 rgba(255,255,255,0.2),
                        inset 0 -1px 0 rgba(0,0,0,0.3),
                        0 2px 4px rgba(0,0,0,0.4)`,
                    transition: 'all 0.2s ease',
                    mixBlendMode: 'normal' as const,
                }}
                onMouseEnter={(e) => {
                    e.currentTarget.style.filter = 'brightness(1.1) contrast(1.1)';
                    e.currentTarget.style.transform = 'translateY(-1px)';
                }}
                onMouseLeave={(e) => {
                    e.currentTarget.style.filter = '';
                    e.currentTarget.style.transform = '';
                }}
                onClick={() => {
                    setGoodButtonClicked(true);
                    good().then(() => {
                    }).catch((e) => {
                        console.error(e.message);
                        // Reset the flag on error so button can be clicked again
                        setGoodButtonClicked(false);
                    });
                }}
            >
                <span style={{
                    color: 'rgba(70, 35, 20, 0.8)',
                    mixBlendMode: 'color-burn',
                    filter: 'contrast(1.2) brightness(0.9)',
                }}>Good</span>
            </button>
            }
        </div>
        }

        {user_id && <CardDiv user_id={user_id} />}

    </div>
};
