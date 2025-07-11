import { PersonalGame, Card } from "../../common/types";
import { useAuth } from "../../contexts/AuthContext";
import { useGame } from "../../contexts/GameContext";
import { useServer } from "../../contexts/ServerContext";
import { VALUE_MAP, SUIT_MAP } from "../../utils/cards";
import { useDrag } from "../../contexts/DragContext";

const DefenderActionPanel = () => {
    const { game, pass, cover } = useServer() as { game: PersonalGame, pass: (cards: Card[]) => Promise<any>, cover: (coverCards: Card[], attackCards: Card[]) => Promise<any> };
    const { coverMap, setCoverMap, selectedCards, setSelectedCards, isSelectingCover, setIsSelectingCover } = useGame();

    // During first attack, defender can do nothing
    if (game.status === 'first_attacker' || game.table_battles.length === 0) {
        return <></>;
    }

    const everyCardUncovered = game.table_battles.every(battle => !battle.defense);
    const someCardUncovered = game.table_battles.some(battle => !battle.defense);

    return <>
        {/* Pass is only shown if no attack card is covered */}
        {everyCardUncovered && <button
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
        }

        {/* Cover is only shown if there are uncovered cards */}
        {someCardUncovered && <button style={{ width: '60px', height: '50px' }} onClick={() => {
            // If there's exactly 1 uncovered card, cover it immediately
            const uncoveredBattles = game.table_battles.filter(battle => !battle.defense);
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
        }

        {/* Actually Cover is shown when in cover selection mode OR when there are covers queued */}
        {(isSelectingCover || coverMap.size > 0) && <>
            <button style={{ width: '60px', height: '100%', fontSize: '10px' }} onClick={() => {
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
    const { game, attack } = useServer() as { game: PersonalGame, attack: (cards: Card[]) => Promise<any> };
    const { selectedCards, setSelectedCards } = useGame();
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

const CardDiv = () => {
    const { game } = useServer() as { game: PersonalGame };

    const { localHandOrder, selectedCards } = useGame();

    const { draggedCardIndex, isDraggingForGameAction, startCardDrag, isActuallyDragging } = useDrag();

    if (!game.self) {
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

        // Determine the style based on game
        let cardStyle: React.CSSProperties;
        if (isDraggingForAction || isSelected) {
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

        return <div
            key={'' + card.value + card.suit}
            data-card-index={index}
            draggable={true}
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
    })} </div>
}

export const ActionButtons = () => {
    const { user_id } = useAuth();
    const { game, pickup, good } = useServer() as { game: PersonalGame, pickup: () => Promise<any>, good: () => Promise<any> };

    const { selectedCards, setSelectedCards } = useGame();

    const self_index = game.players.findIndex((player) => player.player_id === user_id);
    const isDefending = game.defender === self_index;

    return <div 
        data-touch-interactive
        style={{ display: 'flex', flexDirection: 'column', position: 'absolute', bottom: '10px', left: '0px', right: '0px', justifyContent: 'end', alignItems: 'center', height: '200px' }}>
        {/* Always visible pickup and good buttons */}
        {game.self && <div 
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
            {!isDefending && game.table_battles.length > 0 && game.table_battles.every(battle => battle.defense) && <button
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
            game.self && selectedCards.length > 0 && <div 
                data-touch-interactive
                style={{ backgroundColor: 'rgba(0, 0, 0, 0.5)', zIndex: 999, height: '50px ' }}>

                {
                    isDefending ? <DefenderActionPanel /> : <AttackerActionPanel />
                }

            </div>
        }
        <CardDiv />

    </div>
};