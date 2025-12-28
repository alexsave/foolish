import { Card, PersonalGame } from "../../common/types";
import { useAuth } from "../../contexts/AuthContext";
import { useServer } from "../../contexts/ServerContext";
import { useAnimation } from "../../contexts/AnimationContext";
import { useGame } from "../../contexts/GameContext";
import { useDrag } from "../../contexts/DragContext";
import { CardFace} from "./CardFace";
import { useWoodStyle } from "../WoodTexture";
import { useState, useEffect, useRef } from "react";
import { Text } from "../Text";
import { canCover as canCoverUtil } from "../../common/common_utils";
import { canAttack, canPass, canCoverCards } from "../../utils/gameValidation";

const CardDiv = ({ user_id }: { user_id: string }) => {
    const { game, localHandOrder } = useServer() as { game: PersonalGame, localHandOrder: Card[] };

    const { selectedCards } = useGame();

    const { draggedCardIndex, isDraggingForGameAction, startCardDrag, isActuallyDragging } = useDrag();

    if (!game || !game.self) {
        return <p style={{ color: 'white', fontSize: '18px' }}><Text id="spectating" /></p>
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
    const { pickup, good, attack, pass, cover } = useAnimation();

    const { selectedCards, setSelectedCards } = useGame();
    
    // Track if "good" button was just clicked to hide it immediately
    const [goodButtonClicked, setGoodButtonClicked] = useState(false);
    // Track if "attack" button was just clicked to hide it immediately
    const [attackButtonClicked, setAttackButtonClicked] = useState(false);
    
    // Track previous state of shouldShowGoodButton to detect transitions
    const prevShouldShowGoodButtonRef = useRef(false);
    const prevShouldShowAttackButtonRef = useRef(false);
    
    // Use wood texture hooks at the top level
    const woodStylePickup = useWoodStyle(0.15);
    const woodStyleGood = useWoodStyle(0.85);
    const woodStyleAttack = useWoodStyle(0.25);
    const woodStylePass = useWoodStyle(0.35);
    const woodStyleCover = useWoodStyle(0.45);

    // Calculate values needed for the effect (safe even if game is null)
    const self_index = game?.players.findIndex((player) => player.player_id === user_id) ?? -1;
    const isDefending = game && self_index !== -1 ? game.defender === self_index : false;
    const shouldShowGoodButton = !isDefending && 
        (game?.table_battles.length ?? 0) > 0 && 
        (game?.table_battles.every(battle => battle.defense) ?? false) &&
        !(game?.good_players?.includes(user_id ?? '') ?? false); // Don't show if already said good
    
    // Calculate if action buttons should be shown
    const shouldShowAttackButton = game && !isDefending && canAttack(game, selectedCards) && !attackButtonClicked;
    const shouldShowPassButton = game && isDefending && canPass(game, selectedCards);
    const shouldShowCoverButton = game && isDefending && canCoverCards(game, selectedCards);

    // Reset the clicked flags only when transitioning from NOT possible to possible
    useEffect(() => {
        const prevShouldShowGood = prevShouldShowGoodButtonRef.current;
        const prevShouldShowAttack = prevShouldShowAttackButtonRef.current;
        
        // If good wasn't possible before but is now possible, reset the flag
        if (!prevShouldShowGood && shouldShowGoodButton && goodButtonClicked) {
            setGoodButtonClicked(false);
        }
        
        // If attack wasn't possible before but is now possible, reset the flag
        const shouldShowAttackButtonRaw = game && !isDefending && canAttack(game, selectedCards);
        if (!prevShouldShowAttack && shouldShowAttackButtonRaw && attackButtonClicked) {
            setAttackButtonClicked(false);
        }
        
        // Update the refs for next render
        prevShouldShowGoodButtonRef.current = shouldShowGoodButton;
        prevShouldShowAttackButtonRef.current = shouldShowAttackButtonRaw;
    }, [shouldShowGoodButton, goodButtonClicked, game, isDefending, selectedCards, attackButtonClicked]);

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


    const handleAttackClick = () => {
        setAttackButtonClicked(true);
        attack(selectedCards).then(() => {
            setSelectedCards([]);
        }).catch((e) => {
            console.error('Attack failed:', e.message);
            setAttackButtonClicked(false);
        });
    };

    const handlePassClick = () => {
        pass(selectedCards).then(() => {
            setSelectedCards([]);
        }).catch((e) => {
            console.error('Pass failed:', e.message);
        });
    };

    const handleCoverClick = () => {
        // Find the valid cover mapping
        const uncoveredBattles = game.table_battles.filter(battle => !battle.defense);
        const uncoveredAttacks = uncoveredBattles.map(battle => battle.attack);
        
        if (selectedCards.length === 1) {
            // Single card cover - we already verified there's exactly one valid target
            const validTarget = uncoveredBattles.find(battle => 
                canCoverUtil(battle.attack, selectedCards[0], game.power_suit)
            );
            if (validTarget) {
                cover([selectedCards[0]], [validTarget.attack]).then(() => {
                    setSelectedCards([]);
                }).catch((e) => {
                    console.error('Cover failed:', e.message);
                });
            }
        } else {
            // Multi-card cover - find the unambiguous valid mapping
            const findUnambiguousCoverMapping = (coverCards: Card[], uncoveredAttacks: Card[]): { coverCards: Card[], attackCards: Card[] } | null => {
                const combinations: { coverCards: Card[], attackCards: Card[] }[] = [];
                
                const generatePermutations = (arr: Card[], length: number): Card[][] => {
                    if (length === 1) return arr.map(item => [item]);
                    
                    const result: Card[][] = [];
                    for (let i = 0; i < arr.length; i++) {
                        const rest = arr.slice(0, i).concat(arr.slice(i + 1));
                        const subPermutations = generatePermutations(rest, length - 1);
                        for (const subPerm of subPermutations) {
                            result.push([arr[i], ...subPerm]);
                        }
                    }
                    return result;
                };

                const attackPermutations = generatePermutations(uncoveredAttacks, coverCards.length);
                
                for (const attackPerm of attackPermutations) {
                    const isValidCombination = coverCards.every((coverCard, index) => 
                        canCoverUtil(attackPerm[index], coverCard, game.power_suit)
                    );
                    
                    if (isValidCombination) {
                        combinations.push({ coverCards: [...coverCards], attackCards: [...attackPerm] });
                    }
                }

                if (combinations.length === 0) return null;
                
                // Check if all valid combinations cover the same set of attacks
                const cardToString = (card: Card) => `${card.value}-${card.suit}`;
                const firstCombinationAttackSet = new Set(combinations[0].attackCards.map(cardToString));
                
                const allCombinationsHaveSameAttackSet = combinations.every(combo => {
                    const comboAttackSet = new Set(combo.attackCards.map(cardToString));
                    return comboAttackSet.size === firstCombinationAttackSet.size && 
                           Array.from(comboAttackSet).every(cardStr => firstCombinationAttackSet.has(cardStr));
                });
                
                // Return the first combination if unambiguous, null otherwise
                return allCombinationsHaveSameAttackSet ? combinations[0] : null;
            };

            const mapping = findUnambiguousCoverMapping(selectedCards, uncoveredAttacks);
            if (mapping) {
                cover(mapping.coverCards, mapping.attackCards).then(() => {
                    setSelectedCards([]);
                }).catch((e) => {
                    console.error('Multi-card cover failed:', e.message);
                });
            }
        }
    };

    const buttonStyle = {
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
    };

    const buttonTextStyle = {
        color: 'rgba(70, 35, 20, 0.8)',
        mixBlendMode: 'color-burn' as const,
        filter: 'contrast(1.2) brightness(0.9)',
    };

    const spacerStyle = {
        width: '60px',
        height: '40px',
    };

    return <div 
        data-touch-interactive
        style={{ display: 'flex', flexDirection: 'column', position: 'absolute', bottom: '10px', left: '0px', right: '0px', justifyContent: 'end', alignItems: 'center', height: '200px' }}>
        {/* Action buttons positioned on the right */}
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
            {/* For Defenders: [pass slot, pickup, cover slot] */}
            {/* For Attackers: [good slot, attack slot] */}
            
            {isDefending ? (
                <>
                    {/* Pass button or spacer for defenders */}
                    {shouldShowPassButton ? (
                        <button
                            style={{ ...buttonStyle, ...woodStylePass }}
                            onMouseEnter={(e) => {
                                e.currentTarget.style.filter = 'brightness(1.1) contrast(1.1)';
                                e.currentTarget.style.transform = 'translateY(-1px)';
                            }}
                            onMouseLeave={(e) => {
                                e.currentTarget.style.filter = '';
                                e.currentTarget.style.transform = '';
                            }}
                            onClick={handlePassClick}
                        >
                            <span style={buttonTextStyle}><Text id="pass" /></span>
                        </button>
                    ) : (
                        <div style={spacerStyle} />
                    )}

                    {/* Pickup button for defenders (always in same position) */}
                    {game.table_battles.length > 0 && (
                        <button
                            style={{ ...buttonStyle, ...woodStylePickup }}
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
                            <span style={buttonTextStyle}><Text id="pickup" /></span>
                        </button>
                    )}

                    {/* Cover button or spacer for defenders */}
                    {shouldShowCoverButton ? (
                        <button
                            style={{ ...buttonStyle, ...woodStyleCover }}
                            onMouseEnter={(e) => {
                                e.currentTarget.style.filter = 'brightness(1.1) contrast(1.1)';
                                e.currentTarget.style.transform = 'translateY(-1px)';
                            }}
                            onMouseLeave={(e) => {
                                e.currentTarget.style.filter = '';
                                e.currentTarget.style.transform = '';
                            }}
                            onClick={handleCoverClick}
                        >
                            <span style={buttonTextStyle}><Text id="cover" /></span>
                        </button>
                    ) : (
                        <div style={spacerStyle} />
                    )}
                </>
            ) : (
                <>
                    {/* Good button or spacer for attackers */}
                    {shouldShowGoodButton && !goodButtonClicked ? (
                        <button
                            style={{ ...buttonStyle, ...woodStyleGood }}
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
                                    setGoodButtonClicked(false);
                                });
                            }}
                        >
                            <span style={buttonTextStyle}><Text id="good" /></span>
                        </button>
                    ) : (
                        <div style={spacerStyle} />
                    )}

                    {/* Attack button or spacer for attackers */}
                    {shouldShowAttackButton ? (
                        <button
                            style={{ ...buttonStyle, ...woodStyleAttack }}
                            onMouseEnter={(e) => {
                                e.currentTarget.style.filter = 'brightness(1.1) contrast(1.1)';
                                e.currentTarget.style.transform = 'translateY(-1px)';
                            }}
                            onMouseLeave={(e) => {
                                e.currentTarget.style.filter = '';
                                e.currentTarget.style.transform = '';
                            }}
                            onClick={handleAttackClick}
                        >
                            <span style={buttonTextStyle}><Text id="attack" /></span>
                        </button>
                    ) : (
                        <div style={spacerStyle} />
                    )}
                </>
            )}
        </div>
        }

        {user_id && <CardDiv user_id={user_id} />}

    </div>
};
