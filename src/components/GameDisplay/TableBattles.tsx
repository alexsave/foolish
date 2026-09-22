import { CardFace } from "./CardFace";
import { useServer } from "../../contexts/ServerContext";
import { useAuth } from "../../contexts/AuthContext";
import { useGame } from "../../contexts/GameContext";
import { useDrag } from "../../contexts/DragContext";
import { useTutorialHint } from "../../contexts/TutorialHintContext";
import { useAnimation } from "../../contexts/AnimationContext";
// The flight's own timing curve: the card coming down and the card turning out
// from under it are one movement, so they are written with one curve.
import { EASE } from "./FlightCard";
import { covered, sameCard, type ViewCard } from "../../state/view";
import { MOVE_ATTACK, MOVE_COVER, MOVE_PASS } from "@sdk/ts/gen/view_layout.bots.ts";
import type { ClientPlay } from "@sdk/ts/table/client_table.ts";

/** The angle a cover is laid across its attack at, in radians, about the card's
 *  `center bottom`. Exported because the flight that brings the cover in has to
 *  land where this grid will draw it: a cover shares its attack's slot and its
 *  bottom edge, so turning the attack's centre through this angle about that
 *  point IS the cover's centre (AnimationOverlay's `laidAcross`). The overlay
 *  reads the constant rather than keeping an offset of its own. */
export const COVER_ROTATION_RAD: number = Math.PI / 16;

const COVER_ROTATION: string = COVER_ROTATION_RAD + 'rad';

export const TableBattles = () => {
    const game = useServer().view;
    // eslint-disable-next-line @typescript-eslint/no-unused-vars
    const { user_id } = useAuth();
    const { coverMap, setCoverMap, isSelectingCover, selectedCards } = useGame();
    const { isDraggingForGameAction, draggedCard, currentCursorPos, determineGameAction } = useDrag();
    const hint = useTutorialHint();
    // The flight on screen, and how long the kernel says it lasts (its plan's
    // AnimPlanStep.duration_ms). The grid needs both: see `coveringNow`.
    const { currentAnimation, isAnimating, flightMs } = useAnimation();

    // Handle case where game is not loaded yet
    if (!game) {
        return <div></div>;
    }

    // Determine what action would happen if we dropped right now
    let currentAction: ClientPlay | null = null;
    if (isDraggingForGameAction && draggedCard && currentCursorPos) {
        currentAction = determineGameAction(currentCursorPos.x, currentCursorPos.y, draggedCard);
    }

    // Would the current drop cover this attack? The kernel's move names the
    // attacks it covers, so a single cover and a multi-cover are one case.
    const isCardBeingCovered = (attackCard: { suit: number; value: number }) =>
        currentAction?.moveType === MOVE_COVER
        && currentAction.attackCards.some((c) => sameCard(c, attackCard));

    // IS A COVER IN THE AIR FOR THIS BATTLE RIGHT NOW?
    //
    // The attack turns out of the way as the card covering it LEAVES THE HAND,
    // not once that card has landed: the two rotate together, over the one
    // flight, on the one curve. iMessage wrote this rule down after the same bug
    // - `coverTilted` is true while the cover is `flyingNow`, animated
    // `.timingCurve(0.25, 0.46, 0.45, 0.94, duration: flightTime)`
    // (ios/FoolishKit/Boards/FBattleGrid.swift) - while the web had the attack
    // starting its turn only after the landing, riding CardFace's generic 0.2s
    // hover transition, which nobody chose for this.
    //
    // WHICH battle is the KERNEL's answer and not a guess: a pushed cover names
    // the attack it answers and its battle index (evwire -> pushSequence), and my
    // own predicted multi-cover names its pairs.
    const coveringNow = (attack: ViewCard, index: number): boolean => {
        const flight = currentAnimation;
        if (!isAnimating || !flight || flight.type !== 'cover') return false;
        if (flight.target_card) return sameCard(flight.target_card, attack);
        if (flight.target_cards) return flight.target_cards.some((c) => sameCard(c, attack));
        return flight.battle_index === index;
    };

    // How many empty drop zones an attack or a pass would need: the cards the
    // kernel's move actually lays.
    const emptyDropZones = currentAction
        && (currentAction.moveType === MOVE_ATTACK || currentAction.moveType === MOVE_PASS)
        ? currentAction.cards.length : 0;

    return <div data-table-container style={{
        display: 'flex',
        flexWrap: 'wrap',
        justifyContent: 'center',
        gap: '10px',
        width: '100%',
        maxWidth: '300px',
        margin: '0 auto'
    }}> {
        game.battles.map((battle, index) => {
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

            // Tutorial: green-outline the attack the learner should cover.
            if (hint?.targetCard &&
                hint.targetCard.suit === battle.attack.suit &&
                hint.targetCard.value === battle.attack.value && !covered(battle)) {
                containerStyle.border = '3px solid #2fcf63';
                containerStyle.boxShadow = '0 0 14px 2px rgba(47,207,99,0.7)';
                containerStyle.borderRadius = '8px';
            }

            // Determine if this battle is covered
            const isCovered = covered(battle);
            // ...and whether it is turning out of the way of a cover that is
            // still in the air (see `coveringNow`): the tilt starts with the
            // flight, not with the landing.
            const tilted = isCovered || coveringNow(battle.attack, index);

            // Card styles with rotation around bottom center
            const attackCardStyle: React.CSSProperties = {
                position: 'absolute',
                bottom: '5px', // Position at bottom of container
                left: '50%', // Center horizontally
                transform: tilted
                    ? `translateX(-50%) rotate(-${COVER_ROTATION})`
                    : 'translateX(-50%)', // Just center if not covered
                transformOrigin: 'center bottom', // Rotate around bottom center of card
                // The cover's own flight, exactly: the kernel's duration for the
                // step on screen and the flight's own curve, so the two cards
                // turn at the same speed. 0ms when nothing is flying, which is
                // the only time this tilt changes without a card causing it.
                transition: `transform ${flightMs}ms ${EASE}`,
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
                    owner="table"
                    style={attackCardStyle}
                    onClick={() => isSelectingCover && setCoverMap(new Map(coverMap.set(selectedCards[0], battle.attack))) }
                />
                {isCovered && (
                    <CardFace 
                        card={battle.defense} 
                        owner="table" 
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
            data-battle-index={game.battles.length + index}
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