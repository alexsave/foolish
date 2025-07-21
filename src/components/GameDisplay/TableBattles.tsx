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
    const { isDraggingForGameAction } = useDrag();
    
    // Handle case where game is not loaded yet
    if (!game || !game.players || !game.table_battles) {
        return <div></div>;
    }
    
    const self_index = game.players.findIndex(p => p.player_id === user_id);

    return <> {
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

            // Add highlighting for valid drop zones during game action drag
            const isDefending = game.defender === self_index;
            const isValidCoverTarget = isDraggingForGameAction && isDefending && !battle.defense;

            if (isValidCoverTarget) {
                containerStyle.border = '3px solid green';
                containerStyle.backgroundColor = 'rgba(0, 255, 0, 0.1)';
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
    </>
};