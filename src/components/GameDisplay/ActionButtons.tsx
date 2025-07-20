import { Card, PersonalGame } from "../../common/types";
import { useAuth } from "../../contexts/AuthContext";
import { useServer } from "../../contexts/ServerContext";
import { useAnimation } from "../../contexts/AnimationContext";
import { VALUE_MAP, SUIT_MAP } from "../../utils/cards";
import { useGame } from "../../contexts/GameContext";
import { useDrag } from "../../contexts/DragContext";
import { CardFace} from "./CardFace";

const DefenderActionPanel = () => {
    const { game } = useServer() as { game: PersonalGame };
    const { cover, pass } = useAnimation();
    const { selectedCards, setSelectedCards, coverMap, setCoverMap, isSelectingCover, setIsSelectingCover } = useGame();

    if (!game) {
        return <div></div>
    }

    const canPass = (card: Card) => {
        const table_battles = game.table_battles;
        if (table_battles.length === 0) return false;

        // Check if the card is part of selected cards
        const isCardSelected = selectedCards.some(selectedCard =>
            selectedCard.value === card.value && selectedCard.suit === card.suit
        );

        // Use all selected cards if the card is selected, otherwise just the card
        const cardsToCheck = isCardSelected && selectedCards.length > 0 ? selectedCards : [card];

        // All cards must have the same value
        if (!cardsToCheck.every(c => c.value === cardsToCheck[0].value)) {
            return false;
        }

        // All table battles must be uncovered (defense === null)
        // All uncovered attacks must have the same value as the cards to check
        return table_battles.every(battle =>
            battle.defense === null && battle.attack.value === cardsToCheck[0].value
        );
    };

    const someCardUncovered = game.table_battles.some(battle => !battle.defense);

    return <>
        {/* Rework conditional logic for pass */}
        {/* Pass is only shown when valid */}
        {someCardUncovered && selectedCards.some(card => canPass(card)) && (
            <button
                style={{ width: '60px', height: '50px' }}
                onClick={() => pass(selectedCards).then(() => {
                    setSelectedCards([]);
                }).catch((e) => {
                    console.error(e.message);
                })}
            >
                Pass
            </button>
        )}

        {someCardUncovered && <button style={{ width: '60px', height: '50px' }} onClick={() => {
            if (isSelectingCover) {
                // Cancel cover mode
                setIsSelectingCover(false);
                setCoverMap(new Map());
            } else {
                // Enter cover mode
                setIsSelectingCover(true);
                setCoverMap(new Map());
            }
        }}>
            {isSelectingCover ? 'Cancel' : 'Cover'}
        </button>}

        {/* Cover mode buttons */}
        {isSelectingCover && <>
            {/* We're in cover mode - show cover actions */}
            <button style={{ width: '60px', height: '100%', fontSize: '10px' }} onClick={() => {
                // Execute the covering action
                const coveringCards = Array.from(coverMap.keys());
                const coveredCards = Array.from(coverMap.values());

                cover(coveringCards, coveredCards).then(() => {
                    setSelectedCards([]);
                    setCoverMap(new Map());
                    setIsSelectingCover(false);
                }).catch((e) => {
                    console.error(e.message);
                });
            }}>Actually Cover</button>

            {/* Cancel Cover button to reset cover selection */}
            <button style={{ width: '60px', height: '50px', fontSize: '10px' }} onClick={() => {
                setIsSelectingCover(false);
                setCoverMap(new Map());
            }}>Cancel Cover</button>
        </>
        }
    </>
}

const AttackerActionPanel = () => {
    const { user_id } = useAuth();
    const { game } = useServer() as { game: PersonalGame };
    const { attack } = useAnimation();
    const { selectedCards, setSelectedCards } = useGame();
    
    if (!game) {
        return <div></div>
    }
    
    const self_index = game.players.findIndex((player) => player.player_id === user_id);

    return <>
        {/* Attack is only shown when valid */}
        {(game.table_battles.length > 0 || self_index === game.first_attacker) && (
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
}

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

    const { selectedCards, setSelectedCards } = useGame();

    // Handle case where game is not loaded yet
    if (!game || !game.self) {
        return <div>
        </div>
    }

    const self_index = game.players.findIndex((player) => player.player_id === user_id);
    const isDefending = game.defender === self_index;

    const isOut = game.self.status === 'out';
    if (isOut) {
        return <div>
        </div>
    }

    // can play if you have any value on the table
    const canPlay = game.self.hand.some(card => game.table_battles.some(battle => battle.attack.value === card.value || battle.defense?.value === card.value));

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
                    width: '60px',
                    height: '40px',
                    fontSize: '12px',
                    backgroundColor: '#ff6b6b',
                    color: 'white',
                    border: 'none',
                    borderRadius: '4px',
                    cursor: 'pointer'
                }}
                onClick={() => pickup().then(() => {
                    setSelectedCards([]);
                }).catch((e) => {
                    console.error(e.message);
                })}
            >
                Pickup
            </button>
            }

            {/* Good button for attackers when all attacks are covered */}
            {!isDefending && game.table_battles.length > 0 && game.table_battles.every(battle => battle.defense) && canPlay && <button
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
                onClick={() => good().then(() => {
                    setSelectedCards([]);
                }).catch((e) => {
                    console.error(e.message);
                })}
            >
                Good
            </button>
            }
        </div>
        }

        {
            game && game.self && selectedCards.length > 0 && <div 
                data-touch-interactive
                style={{ backgroundColor: 'rgba(0, 0, 0, 0.5)', zIndex: 999, height: '50px ' }}>

                {
                    isDefending ? <DefenderActionPanel /> : <AttackerActionPanel />
                }

            </div>
        }
        {user_id && <CardDiv user_id={user_id} />}

    </div>
};
