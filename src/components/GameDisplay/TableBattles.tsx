import { PersonalGame } from "../../common/types";
import { CardFace } from "./CardFace";
import { useServer } from "../../contexts/ServerContext";
import { useAuth } from "../../contexts/AuthContext";
import { useGame } from "../../contexts/GameContext";
import { useDrag } from "../../contexts/DragContext";

const COVER_ROTATION: string = (Math.PI/ 16) + 'rad';

export const TableBattles = () => {
    const game: PersonalGame = useServer().game as PersonalGame;
    const { user_id } = useAuth();
    const { coverMap, setCoverMap, isSelectingCover, selectedCards } = useGame();
    const { isDraggingForGameAction, draggedCard, currentCursorPos, determineGameAction } = useDrag();
    
    // Handle case where game is not loaded yet
    if (!game || !game.players || !game.table_battles) {
        return <div></div>;
    }
    
    const self_index = game.players.findIndex(p => p.player_id === user_id);

    // Determine what action would happen if we dropped right now
    let currentAction: 
        | { type: 'attack' }
        | { type: 'cover', targetCard: any }
        | { type: 'multicover', coverCards: any[], attackCards: any[] }
        | { type: 'pass' }
        | { type: 'rearrange' }
        | { type: 'invalid' }
        | null = null;
    if (isDraggingForGameAction && draggedCard && currentCursorPos) {
        currentAction = determineGameAction(currentCursorPos.x, currentCursorPos.y, draggedCard);
    }

    // Helper function to check if a card would be covered by the current action
    const isCardBeingCovered = (attackCard: any) => {
        if (!currentAction) return false;
        
        if (currentAction.type === 'cover' && currentAction.targetCard) {
            return currentAction.targetCard.value === attackCard.value && 
                   currentAction.targetCard.suit === attackCard.suit;
        }
        
        if (currentAction.type === 'multicover' && currentAction.attackCards) {
            return currentAction.attackCards.some((card: any) => 
                card.value === attackCard.value && card.suit === attackCard.suit
            );
        }
        
        return false;
    };

    // Calculate how many empty drop zones we need for attack/pass
    let emptyDropZones = 0;
    if (currentAction && (currentAction.type === 'attack' || currentAction.type === 'pass')) {
        // Check if the dragged card is part of selected cards
        const isDraggedCardSelected = selectedCards.some(selectedCard =>
            selectedCard.value === draggedCard!.value && selectedCard.suit === draggedCard!.suit
        );
        
        // Use all selected cards if the dragged card is selected, otherwise just the dragged card
        const cardsToUse = isDraggedCardSelected && selectedCards.length > 0 ? selectedCards : [draggedCard];
        emptyDropZones = cardsToUse.length;
    }

    return <div style={{
        display: 'flex',
        flexWrap: 'wrap',
        justifyContent: 'center',
        gap: '10px',
        width: '100%',
        maxWidth: '300px',
        margin: '0 auto'
    }}> {
        game.table_battles.map((battle, index) => {
            let containerStyle: React.CSSProperties = {
                display: 'flex',
                flexDirection: 'column',
                alignItems: 'center',
                position: 'relative',
                width: '60px', // Slightly wider to accommodate rotation
                height: '80px', // Taller to accommodate stacked/rotated cards
                margin: '5px',
                justifyContent: 'center',
            };

            if (Array.from(coverMap.values()).some(c => c.value === battle.attack.value && c.suit === battle.attack.suit)) {
                containerStyle.border = '3px solid red';
            }

            // Highlight cards that would be covered
            if (isCardBeingCovered(battle.attack)) {
                containerStyle.border = '3px solid #d29002';
                containerStyle.backgroundColor = 'rgba(210, 144, 2, 0.1)';
            }

            // Determine if this battle is covered
            const isCovered = !!battle.defense;

            // Card styles with rotation around bottom center
            const attackCardStyle: React.CSSProperties = {
                position: 'absolute',
                bottom: '5px', // Position at bottom of container
                left: '50%', // Center horizontally
                transform: isCovered 
                    ? `translateX(-50%) rotate(-${COVER_ROTATION})` 
                    : 'translateX(-50%)', // Just center if not covered
                transformOrigin: 'center bottom', // Rotate around bottom center of card
                zIndex: isCovered ? 1 : 2, // Attack goes behind when covered
            };

            const defenseCardStyle: React.CSSProperties = {
                position: 'absolute',
                bottom: '5px', // Position at bottom of container
                left: '50%', // Center horizontally  
                transform: `translateX(-50%) rotate(${COVER_ROTATION})`,
                transformOrigin: 'center bottom', // Rotate around bottom center of card
                zIndex: 2, // Defense always on top when present
            };

            return <div key={battle.attack.value + ' ' + battle.attack.suit} style={containerStyle} data-location="table">
                <CardFace
                    data-battle-index={index}
                    data-card={`${battle.attack.suit}-${battle.attack.value}`}
                    card={battle.attack}
                    playerId="table"
                    style={attackCardStyle}
                    onClick={() => isSelectingCover && setCoverMap(new Map(coverMap.set(selectedCards[0], battle.attack))) }
                />
                {battle.defense && (
                    <CardFace 
                        card={battle.defense} 
                        playerId="table" 
                        style={defenseCardStyle}
                        data-card={`${battle.defense.suit}-${battle.defense.value}`} 
                    />
                )}
            </div>
        })
    }
    {/* Render empty drop zones for attack/pass actions */}
    {Array.from({ length: emptyDropZones }, (_, index) => (
        <div 
            key={`empty-${index}`} 
            data-location="table"
            data-battle-index={game.table_battles.length + index}
            style={{
                display: 'flex',
                flexDirection: 'column',
                alignItems: 'center',
                position: 'relative',
                width: '60px',
                height: '80px',
                margin: '5px',
                justifyContent: 'center',
                border: '3px dashed #d29002',
                backgroundColor: 'rgba(210, 144, 2, 0.1)',
                borderRadius: '8px'
            }}
        >
            {/* Empty placeholder for new card */}
            <div style={{
                width: '40px',
                height: '60px',
                border: '2px dashed #d29002',
                borderRadius: '4px',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
                fontSize: '24px',
                color: '#d29002',
                opacity: 0.7
            }}>
                +
            </div>
        </div>
    ))}
    </div>
};