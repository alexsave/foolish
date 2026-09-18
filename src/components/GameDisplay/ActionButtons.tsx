import { useAuth } from "../../contexts/AuthContext";
import { useServer } from "../../contexts/ServerContext";
import { useAnimation } from "../../contexts/AnimationContext";
import { useGame } from "../../contexts/GameContext";
import { useDrag } from "../../contexts/DragContext";
import { CardFace } from "./CardFace";
import { TexturedSurface } from "../TexturedSurface";
import { useEffect, useRef } from "react";
import { Text } from "../Text";
import { canAttack, canPass, canCoverCards, canPickup, coverGesture } from "../../utils/gameValidation";
import { useStyles } from "../../contexts/StyleContext";
import { useTutorialHint } from "../../contexts/TutorialHintContext";
import { PLAYER_STATUS, rulesOf, seatKey } from "../../state/view";

// Green glow used by the tutorial to point at the card/button to use next.
const TUT_GLOW = '0 0 0 3px #2fcf63, 0 0 16px 3px rgba(47,207,99,0.85)';

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

const CardDiv = () => {
    const { view: game, localHandOrder } = useServer();
    const { selectedCards } = useGame();
    const { draggedCardIndex, isDraggingForGameAction, startCardDrag, isActuallyDragging } = useDrag();
    const styles = useStyles();
    const hint = useTutorialHint();

    if (!game || game.mySeat < 0) {
        return <p style={{ color: 'var(--color-text-primary)', fontSize: '18px' }}><Text id="spectating" /></p>;
    }
    // The page's name for my hand, which flights and the keyboard find it by.
    const handKey = seatKey(game, game.mySeat);

    return (
        <div 
            data-touch-interactive
            data-hand-container
            data-player-id={handKey}
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

                const isHinted = !!hint?.cards.some(
                    h => h.suit === card.suit && h.value === card.value,
                );
                const borderColor = isHinted
                    ? '#2fcf63'
                    : (isDraggingForAction || isSelected)
                    ? styles.cardInHand.selectedBorderColor
                    : styles.cardInHand.borderColor;

                return (
                    <CardFace
                        card={card}
                        owner={game.mySeat}
                        key={'' + card.value + card.suit}
                        data-card-index={index}
                        data-location="hand"
                        data-player-id={handKey}
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
                            // visibility switches at once: a card a flight carries is hidden
                            // here (CardFace), and a transition's first frame would still hide
                            // it after the flight has landed.
                            transition: 'all 0.1s ease, visibility 0s',
                            transform: isHinted ? 'translateY(-10px)' : undefined,
                            cursor: 'move',
                            userSelect: 'none',
                            margin: '0 1px',
                            border: `2px solid ${borderColor}`,
                            boxShadow: isHinted ? TUT_GLOW : styles.cardInHand.boxShadow,
                        }}
                    />
                );
            })}
        </div>
    );
};

// Wraps a wooden action button with the tutorial's green glow + a stable
// test hook when it is the move the learner should make next.
const Glow = ({ on, children }: { on: boolean; children: React.ReactNode }) => (
    <div data-testid={on ? 'tut-move' : undefined} style={on ? { borderRadius: 10, boxShadow: TUT_GLOW } : undefined}>
        {children}
    </div>
);

export const ActionButtons = () => {
    const { user_id } = useAuth();
    const { view: game } = useServer();
    const { pickup, good, attack, pass, cover } = useAnimation();
    const { selectedCards, setSelectedCards, pressedActions, setActionPressed } = useGame();
    const hint = useTutorialHint();

    const self_index = game?.mySeat ?? -1;
    const isDefending = game && self_index !== -1 ? game.defender === self_index : false;

    // raw "this button is relevant" predicates, ignoring the optimistic pressed
    // flag. The rendered button additionally requires !pressedActions[name], so a
    // press (click OR keyboard) hides it immediately until the server catches up.
    // Whether Good is offered is the kernel's (client_view_rules).
    //
    // TODO(ios-parity): iMessage board hides Take while cards are selected
    // (defender) and Good while cards are selected (attacker); consider matching
    // here.
    const rawGood = !!game && rulesOf(game).canSayGood;
    const rawAttack = !!(game && !isDefending && canAttack(game, selectedCards));
    const rawPass = !!(game && isDefending && canPass(game, selectedCards));
    const rawCover = !!(game && isDefending && canCoverCards(game, selectedCards));
    const rawPickup = !!(game && canPickup(game));

    const shouldShowGoodButton = rawGood && !pressedActions['good'];
    const shouldShowAttackButton = rawAttack && !pressedActions['attack'];
    const shouldShowPassButton = rawPass && !pressedActions['pass'];
    const shouldShowCoverButton = rawCover && !pressedActions['cover'];
    const shouldShowPickupButton = rawPickup && !pressedActions['pickup'];

    // when a button becomes legitimately relevant again (raw rising edge), drop
    // its stale optimistic flag so it can re-show on the next turn.
    const prevRaw = useRef<Record<string, boolean>>({});
    useEffect(() => {
        const raws: Record<string, boolean> = {
            good: rawGood, attack: rawAttack, pass: rawPass, cover: rawCover, pickup: rawPickup,
        };
        for (const a of Object.keys(raws)) {
            if (!prevRaw.current[a] && raws[a] && pressedActions[a]) setActionPressed(a, false);
            prevRaw.current[a] = raws[a];
        }
    }, [rawGood, rawAttack, rawPass, rawCover, rawPickup, pressedActions, setActionPressed]);

    if (!game || game.mySeat < 0) {
        return <div></div>;
    }

    const isOut = game.seats[game.mySeat].status === PLAYER_STATUS.OUT;
    if (isOut) {
        return <div></div>;
    }

    // A move spends the selection when it is sent: its cards leave the hand, and a
    // refused card comes home unselected. Clearing it when the server answers
    // instead left a refused card selected (the next pick mixed with it and the
    // Attack button vanished) and wiped a pick made while the move was on its way.
    const handleAttackClick = () => {
        setActionPressed('attack', true);
        setSelectedCards([]);
        attack(selectedCards).catch((e) => {
            console.error('Attack failed:', e.message);
            setActionPressed('attack', false);
        });
    };

    const handlePassClick = () => {
        setActionPressed('pass', true);
        setSelectedCards([]);
        pass(selectedCards).catch((e) => {
            console.error('Pass failed:', e.message);
            setActionPressed('pass', false);
        });
    };

    // The button aims itself: which battle it covers, and with which cards, is
    // the kernel's answer (gameValidation.coverGesture -> client_play with
    // CLIENT_PLAY_COVER_BUTTON). The same answer decided the button was live.
    const handleCoverClick = () => {
        const move = coverGesture(game, selectedCards);
        if (!move) return;
        setActionPressed('cover', true);
        setSelectedCards([]);
        cover([...move.cards], [...move.attackCards]).catch((e) => {
            console.error('Cover failed:', e.message);
            setActionPressed('cover', false);
        });
    };

    const spacerStyle = { width: '60px', height: '40px' };

    return (
        <div 
            data-touch-interactive
            style={{ display: 'flex', flexDirection: 'column', position: 'absolute', bottom: 'max(10px, env(safe-area-inset-bottom))', left: '0px', right: '0px', justifyContent: 'end', alignItems: 'center', height: '200px' }}
        >
            {game && game.mySeat >= 0 && (
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
                                <Glow on={hint?.action === 'pass'}>
                                    <ActionButton seed={0.35} onClick={handlePassClick}>
                                        <Text id="pass" />
                                    </ActionButton>
                                </Glow>
                            ) : (
                                <div style={spacerStyle} />
                            )}

                            {shouldShowPickupButton && (
                                <Glow on={hint?.action === 'pickup'}>
                                    <ActionButton seed={0.15} onClick={() => {
                                        setActionPressed('pickup', true);
                                        pickup().catch((e) => {
                                            console.error(e.message);
                                            setActionPressed('pickup', false);
                                        });
                                    }}>
                                        <Text id="pickup" />
                                    </ActionButton>
                                </Glow>
                            )}

                            {shouldShowCoverButton ? (
                                <Glow on={hint?.action === 'cover'}>
                                    <ActionButton seed={0.45} onClick={handleCoverClick}>
                                        <Text id="cover" />
                                    </ActionButton>
                                </Glow>
                            ) : (
                                <div style={spacerStyle} />
                            )}
                        </>
                    ) : (
                        <>
                            {shouldShowGoodButton ? (
                                <Glow on={hint?.action === 'good'}>
                                    <ActionButton seed={0.85} onClick={() => {
                                        setActionPressed('good', true);
                                        good().catch((e) => {
                                            console.error(e.message);
                                            setActionPressed('good', false);
                                        });
                                    }}>
                                        <Text id="good" />
                                    </ActionButton>
                                </Glow>
                            ) : (
                                <div style={spacerStyle} />
                            )}

                            {shouldShowAttackButton ? (
                                <Glow on={hint?.action === 'attack'}>
                                    <ActionButton seed={0.25} onClick={handleAttackClick}>
                                        <Text id="attack" />
                                    </ActionButton>
                                </Glow>
                            ) : (
                                <div style={spacerStyle} />
                            )}
                        </>
                    )}
                </div>
            )}

            {/* a signed-in watcher is told they are watching; a seat holds a hand, signed in or not (the tutorial) */}
            {(user_id || (game && game.mySeat >= 0)) && <CardDiv />}
        </div>
    );
};
