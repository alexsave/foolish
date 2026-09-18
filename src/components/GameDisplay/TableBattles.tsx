import { CardFace } from "./CardFace";
import { useServer } from "../../contexts/ServerContext";
import { useAuth } from "../../contexts/AuthContext";
import { useGame } from "../../contexts/GameContext";
import { useDrag } from "../../contexts/DragContext";
import { useTutorialHint } from "../../contexts/TutorialHintContext";
import { covered, sameCard } from "../../state/view";
import { MOVE_ATTACK, MOVE_COVER, MOVE_PASS } from "@sdk/ts/gen/view_layout.bots.ts";
import type { ClientPlay } from "@sdk/ts/table/client_table.ts";

const COVER_ROTATION: string = (Math.PI/ 16) + 'rad';

export const TableBattles = () => {
    const game = useServer().view;
    // eslint-disable-next-line @typescript-eslint/no-unused-vars
    const { user_id } = useAuth();
    const { coverMap, setCoverMap, isSelectingCover, selectedCards } = useGame();
    const { isDraggingForGameAction, draggedCard, currentCursorPos, determineGameAction } = useDrag();
    const hint = useTutorialHint();

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