import { Card, PersonalGame, PLAYER_STATUS } from "../../common/types";
import { useAuth } from "../../contexts/AuthContext";
import { useServer } from "../../contexts/ServerContext";
import { useAnimation } from "../../contexts/AnimationContext";
import { useGame } from "../../contexts/GameContext";
import { useDrag } from "../../contexts/DragContext";
import { CardFace } from "./CardFace";
import { TexturedSurface } from "../TexturedSurface";
import { useState, useEffect, useRef } from "react";
import { Text } from "../Text";
import { canCover as canCoverUtil } from "../../common/common_utils";
import { canAttack, canPass, canCoverCards } from "../../utils/gameValidation";
import { useStyles } from "../../contexts/StyleContext";

interface ActionButtonProps {
    seed: number;
    onClick: () => void;
    children: React.ReactNode;
}

const ActionButton: React.FC<ActionButtonProps> = ({ seed, onClick, children }) => {
    return (
        <TexturedSurface
            as="button"
            seed={seed}
            onClick={onClick}
            className="btn-action"
            onMouseEnter={(e: React.MouseEvent<HTMLButtonElement>) => {
                e.currentTarget.style.filter = 'brightness(1.1) contrast(1.1)';
                e.currentTarget.style.transform = 'translateY(-1px)';
            }}
            onMouseLeave={(e: React.MouseEvent<HTMLButtonElement>) => {
                e.currentTarget.style.filter = '';
                e.currentTarget.style.transform = '';
            }}
        >
            <span className="btn-action-text">{children}</span>
        </TexturedSurface>
    );
};

const CardDiv = ({ user_id }: { user_id: string }) => {
    const { game, localHandOrder } = useServer() as { game: PersonalGame, localHandOrder: Card[] };
    const { selectedCards } = useGame();
    const { draggedCardIndex, isDraggingForGameAction, startCardDrag, isActuallyDragging } = useDrag();
    const styles = useStyles();

    if (!game || !game.self) {
        return <p style={{ color: 'var(--color-text-primary)', fontSize: '18px' }}><Text id="spectating" /></p>;
    }

    return (
        <div 
            data-touch-interactive
            style={{
                display: 'flex',
                flexDirection: 'row',
                alignItems: 'center',
                justifyContent: 'center',
                width: '100%'
            }}
        >
            {localHandOrder.map((card, index) => {
                const isSelected = selectedCards.some(selectedCard =>
                    selectedCard.value === card.value && selectedCard.suit === card.suit
                );
                const isDragging = isActuallyDragging && draggedCardIndex === index;
                const isDraggingForAction = isDraggingForGameAction && draggedCardIndex === index;

                const borderColor = (isDraggingForAction || isSelected) 
                    ? styles.cardInHand.selectedBorderColor 
                    : styles.cardInHand.borderColor;

                return (
                    <CardFace
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
                        onClick={() => true}
                        style={{
                            flex: '1 1 0',
                            minWidth: '20px',
                            maxWidth: '50px',
                            zIndex: 1000,
                            height: '70px',
                            borderRadius: styles.cardInHand.borderRadius,
                            display: 'flex',
                            justifyContent: 'center',
                            alignItems: 'center',
                            opacity: (isDragging && !isDraggingForAction) ? 0.3 : 1,
                            transition: 'all 0.1s ease',
                            cursor: 'move',
                            userSelect: 'none',
                            margin: '0 1px',
                            border: `2px solid ${borderColor}`,
                            boxShadow: styles.cardInHand.boxShadow,
                        }}
                    />
                );
            })}
        </div>
    );
};

export const ActionButtons = () => {
    const { user_id } = useAuth();
    const { game } = useServer() as { game: PersonalGame };
    const { pickup, good, attack, pass, cover } = useAnimation();
    const { selectedCards, setSelectedCards } = useGame();
    
    const [goodButtonClicked, setGoodButtonClicked] = useState(false);
    const [attackButtonClicked, setAttackButtonClicked] = useState(false);
    
    const prevShouldShowGoodButtonRef = useRef(false);
    const prevShouldShowAttackButtonRef = useRef(false);

    const self_index = game?.players.findIndex((player) => player.player_id === user_id) ?? -1;
    const isDefending = game && self_index !== -1 ? game.defender === self_index : false;
    const shouldShowGoodButton = !isDefending && 
        (game?.table_battles.length ?? 0) > 0 && 
        (game?.table_battles.every(battle => battle.defense) ?? false) &&
        !(game?.good_players?.includes(user_id ?? '') ?? false);
    
    const shouldShowAttackButton = game && !isDefending && canAttack(game, selectedCards) && !attackButtonClicked;
    const shouldShowPassButton = game && isDefending && canPass(game, selectedCards);
    const shouldShowCoverButton = game && isDefending && canCoverCards(game, selectedCards);

    useEffect(() => {
        const prevShouldShowGood = prevShouldShowGoodButtonRef.current;
        const prevShouldShowAttack = prevShouldShowAttackButtonRef.current;
        
        if (!prevShouldShowGood && shouldShowGoodButton && goodButtonClicked) {
            setGoodButtonClicked(false);
        }
        
        const shouldShowAttackButtonRaw = game && !isDefending && canAttack(game, selectedCards);
        if (!prevShouldShowAttack && shouldShowAttackButtonRaw && attackButtonClicked) {
            setAttackButtonClicked(false);
        }
        
        prevShouldShowGoodButtonRef.current = shouldShowGoodButton;
        prevShouldShowAttackButtonRef.current = shouldShowAttackButtonRaw;
    }, [shouldShowGoodButton, goodButtonClicked, game, isDefending, selectedCards, attackButtonClicked]);

    if (!game || !game.self) {
        return <div></div>;
    }

    const isOut = game.self.status === PLAYER_STATUS.OUT;
    if (isOut) {
        return <div></div>;
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
        const uncoveredBattles = game.table_battles.filter(battle => !battle.defense);
        const uncoveredAttacks = uncoveredBattles.map(battle => battle.attack);
        
        if (selectedCards.length === 1) {
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
                
                const cardToString = (card: Card) => `${card.value}-${card.suit}`;
                const firstCombinationAttackSet = new Set(combinations[0].attackCards.map(cardToString));
                
                const allCombinationsHaveSameAttackSet = combinations.every(combo => {
                    const comboAttackSet = new Set(combo.attackCards.map(cardToString));
                    return comboAttackSet.size === firstCombinationAttackSet.size && 
                           Array.from(comboAttackSet).every(cardStr => firstCombinationAttackSet.has(cardStr));
                });
                
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

    const spacerStyle = { width: '60px', height: '40px' };

    return (
        <div 
            data-touch-interactive
            style={{ display: 'flex', flexDirection: 'column', position: 'absolute', bottom: 'max(10px, env(safe-area-inset-bottom))', left: '0px', right: '0px', justifyContent: 'end', alignItems: 'center', height: '200px' }}
        >
            {game && game.self && (
                <div 
                    data-touch-interactive
                    style={{
                        position: 'absolute',
                        bottom: '90px',
                        right: '20px',
                        display: 'flex',
                        flexDirection: 'column',
                        gap: '5px',
                        zIndex: 999
                    }}
                >
                    {isDefending ? (
                        <>
                            {shouldShowPassButton ? (
                                <ActionButton seed={0.35} onClick={handlePassClick}>
                                    <Text id="pass" />
                                </ActionButton>
                            ) : (
                                <div style={spacerStyle} />
                            )}

                            {game.table_battles.length > 0 && (
                                <ActionButton seed={0.15} onClick={() => pickup().catch((e) => console.error(e.message))}>
                                    <Text id="pickup" />
                                </ActionButton>
                            )}

                            {shouldShowCoverButton ? (
                                <ActionButton seed={0.45} onClick={handleCoverClick}>
                                    <Text id="cover" />
                                </ActionButton>
                            ) : (
                                <div style={spacerStyle} />
                            )}
                        </>
                    ) : (
                        <>
                            {shouldShowGoodButton && !goodButtonClicked ? (
                                <ActionButton seed={0.85} onClick={() => {
                                    setGoodButtonClicked(true);
                                    good().catch((e) => {
                                        console.error(e.message);
                                        setGoodButtonClicked(false);
                                    });
                                }}>
                                    <Text id="good" />
                                </ActionButton>
                            ) : (
                                <div style={spacerStyle} />
                            )}

                            {shouldShowAttackButton ? (
                                <ActionButton seed={0.25} onClick={handleAttackClick}>
                                    <Text id="attack" />
                                </ActionButton>
                            ) : (
                                <div style={spacerStyle} />
                            )}
                        </>
                    )}
                </div>
            )}

            {user_id && <CardDiv user_id={user_id} />}
        </div>
    );
};
