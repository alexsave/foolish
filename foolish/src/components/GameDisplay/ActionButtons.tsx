import { useAuth } from "../../contexts/AuthContext";
import { useServer } from "../../contexts/ServerContext";
import { useAnimation } from "../../contexts/AnimationContext";
import { useGame } from "../../contexts/GameContext";
import { useDrag } from "../../contexts/DragContext";
import { CardFace } from "./CardFace";
import { TexturedSurface } from "../TexturedSurface";
import { useEffect, useRef } from "react";
import { Text } from "../Text";
import { boardPills, coverGesture } from "../../utils/gameValidation";
import { useStyles } from "../../contexts/StyleContext";
import { useTutorialHint } from "../../contexts/TutorialHintContext";
import { slotBottom, useHandShape } from "./handShape";
import { PLAYER_STATUS, seatKey } from "../../state/view";
import {
    PLAY_PILL_ATTACK, PLAY_PILL_COVER, PLAY_PILL_GOOD, PLAY_PILL_PASS, PLAY_PILL_PICKUP,
} from "@sdk/ts/gen/view_layout.bots.ts";

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
    const { layout, planeRef } = useHandShape();

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
            style={{ width: '100%' }}
        >
            {/* THE PLANE the kernel's slots are in (handShape.tsx): as wide as
                the row inside its gutters, as tall as the kernel says the laid-out
                hand stands, and every card placed absolutely at its slot - one
                row or two, the same code. */}
            <div data-hand-plane ref={planeRef} style={{ position: 'relative', width: '100%', height: `${layout.rows.height}px` }}>
            {localHandOrder.map((card, index) => {
                const slot = layout.slot[index];
                if (!slot) return null;
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
                            position: 'absolute',
                            left: `${slot.x}px`,
                            bottom: `${slotBottom(layout, index)}px`,
                            width: `${slot.w}px`,
                            height: `${slot.h}px`,
                            boxSizing: 'border-box',
                            zIndex: 1000,
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
                            border: `2px solid ${borderColor}`,
                            boxShadow: isHinted ? TUT_GLOW : styles.cardInHand.boxShadow,
                        }}
                    />
                );
            })}
            </div>
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

// The action column, top to bottom. It holds the kernel's pills and nothing
// else: no slot is kept for a button that is not there. The kernel answers one
// pill at a time, except a defender's card that both covers and transfers (a
// trump of the attack's rank), which answers Cover and Pass together. The column
// is pinned by its bottom edge, so a lone pill always stands in the same place
// and a second one grows upward. The order is the iMessage column's (FActionBar's
// VStack, bottom-anchored in MessageTableView): Cover above Pass, so Pass keeps
// the slot every lone pill uses.
type PillName = 'attack' | 'cover' | 'pass' | 'pickup' | 'good';
const COLUMN: readonly { name: PillName; bit: number; seed: number }[] = [
    { name: 'attack', bit: PLAY_PILL_ATTACK, seed: 0.25 },
    { name: 'cover', bit: PLAY_PILL_COVER, seed: 0.45 },
    { name: 'pass', bit: PLAY_PILL_PASS, seed: 0.35 },
    { name: 'pickup', bit: PLAY_PILL_PICKUP, seed: 0.15 },
    { name: 'good', bit: PLAY_PILL_GOOD, seed: 0.85 },
];

export const ActionButtons = () => {
    const { user_id } = useAuth();
    const { view: game } = useServer();
    const { pickup, good, attack, pass, cover } = useAnimation();
    const { selectedCards, setSelectedCards, pressedActions, setActionPressed } = useGame();
    const hint = useTutorialHint();
    // A hand of two rows stands taller than one, and the box, the pills above
    // it and my seat badge (PlayerRing) all rise by exactly that much.
    const { lift } = useHandShape();

    // Which buttons this selection offers is the kernel's answer (legal.h
    // play_pills, the rule the iMessage board draws by): one move, one
    // button, so a card under the finger takes Take and Good away. The rendered
    // button additionally requires !pressedActions[name], so a press (click OR
    // keyboard) hides it immediately until the server catches up.
    const pills = game ? boardPills(game, selectedCards) : 0;

    // when a button becomes legitimately relevant again (a rising edge), drop
    // its stale optimistic flag so it can re-show on the next turn. The edge is
    // read with the empty selection's pills folded in: picking a card up and
    // putting it down again takes Good or Take away and brings it back, and that
    // must not re-arm a press still on its way to the server.
    const rearm = pills | (game ? boardPills(game, []) : 0);
    const prevRearm = useRef(0);
    useEffect(() => {
        for (const { name, bit } of COLUMN) {
            if (!(prevRearm.current & bit) && (rearm & bit) && pressedActions[name]) setActionPressed(name, false);
        }
        prevRearm.current = rearm;
    }, [rearm, pressedActions, setActionPressed]);

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

    const handlePickupClick = () => {
        setActionPressed('pickup', true);
        pickup().catch((e) => {
            console.error(e.message);
            setActionPressed('pickup', false);
        });
    };

    const handleGoodClick = () => {
        setActionPressed('good', true);
        good().catch((e) => {
            console.error(e.message);
            setActionPressed('good', false);
        });
    };

    const onClick: Record<PillName, () => void> = {
        attack: handleAttackClick, cover: handleCoverClick, pass: handlePassClick,
        pickup: handlePickupClick, good: handleGoodClick,
    };

    return (
        <div 
            data-touch-interactive
            style={{ display: 'flex', flexDirection: 'column', position: 'absolute', bottom: 'max(10px, env(safe-area-inset-bottom))', left: '0px', right: '0px', justifyContent: 'end', alignItems: 'center', height: `${200 + lift}px` }}
        >
            <div 
                data-touch-interactive
                style={{
                    position: 'absolute',
                    bottom: `${90 + lift}px`,
                    right: '20px',
                    display: 'flex',
                    flexDirection: 'column',
                    gap: '5px',
                    zIndex: 999
                }}
            >
                {COLUMN.filter(({ name, bit }) => (pills & bit) !== 0 && !pressedActions[name]).map(({ name, seed }) => (
                    <Glow key={name} on={hint?.action === name}>
                        <ActionButton seed={seed} onClick={onClick[name]}>
                            <Text id={name} />
                        </ActionButton>
                    </Glow>
                ))}
            </div>

            {/* a signed-in watcher is told they are watching; a seat holds a hand, signed in or not (the tutorial) */}
            {(user_id || (game && game.mySeat >= 0)) && <CardDiv />}
        </div>
    );
};
