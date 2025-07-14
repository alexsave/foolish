import { PersonalGame } from "../../common/types";
import { CardFace } from "./CardFace";
import { useServer } from "../../contexts/ServerContext";
import { useAuth } from "../../contexts/AuthContext";
import { useGame } from "../../contexts/GameContext";
import { useDrag } from "../../contexts/DragContext";

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
                flexDirection: 'row',
                alignItems: 'center',
                justifyContent: 'center'
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

            return <div key={battle.attack.value + ' ' + battle.attack.suit} style={containerStyle}>
                <CardFace
                    data-battle-index={index}
                    card={battle.attack}
                    onClick={() => isSelectingCover && setCoverMap(new Map(coverMap.set(selectedCards[0], battle.attack))) }
                />
                {battle.defense && <CardFace card={battle.defense} />}
            </div>
        })
    }
    </>
};