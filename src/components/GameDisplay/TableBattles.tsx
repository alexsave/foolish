import { PersonalGame } from "../../common/types";
import { CardFace } from "./CardFace";
import { useServer } from "../../contexts/ServerContext";
import { useAuth } from "../../contexts/AuthContext";
import { useGame } from "../../contexts/GameContext";
import { useDrag } from "../../contexts/DragContext";

export const TableBattles = () => {
    const game: PersonalGame = useServer().game as PersonalGame;
    const { user_id } = useAuth();
    const self_index = game.players.findIndex(p => p.player_id === user_id);

    const { coverMap, setCoverMap, isSelectingCover, selectedCards } = useGame();

    const {isDraggingForGameAction} = useDrag();

    return <>
        {
            game.table_battles.map((battle, index) => {
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
                const isDefending = game.defender === self_index;
                const isValidCoverTarget = isDraggingForGameAction && isDefending && !battle.defense;

                if (isValidCoverTarget) {
                    containerStyle.border = '3px solid green';
                    containerStyle.backgroundColor = 'rgba(0, 255, 0, 0.1)';
                }

                return <div key={battle.attack.value + ' ' + battle.attack.suit} style={containerStyle}>
                    <div data-battle-index={index}>
                        <CardFace
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
                    {battle.defense && <CardFace card={battle.defense} />}
                </div>
            })
        }
    </>
};