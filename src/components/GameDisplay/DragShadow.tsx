import { useDrag } from "../../contexts/DragContext";
import { useGame } from "../../contexts/GameContext";
import { CardFace } from "./CardFace";
import { SovietIcon } from "../SovietIcon";
import { useStyles } from "../../contexts/StyleContext";
import { useLocalization } from "../../contexts/LocalizationContext";
import { MOVE_ATTACK, MOVE_COVER, MOVE_PASS } from "@sdk/ts/gen/view_layout.bots.ts";

export const DragShadow = () => {
    const { selectedCards } = useGame();
    const { isDraggingForGameAction, draggedCard, currentCursorPos, determineGameAction } = useDrag();
    const styles = useStyles();
    const { t } = useLocalization();

    // The badge under the cursor names the kernel's MOVE_*, not a word this
    // file invented for it.
    const getIndicatorContent = (moveType: number) => {
        switch (moveType) {
            case MOVE_ATTACK:
                return <><SovietIcon name="sword" size={16} /> {t('attack')}</>;
            case MOVE_COVER:
                return <><SovietIcon name="shield" size={16} /> {t('cover')}</>;
            case MOVE_PASS:
                return <>{styles.icons.passIcon} {t('pass')}</>;
            default:
                return null;
        }
    };

    if (!isDraggingForGameAction || !draggedCard || !currentCursorPos) {
        return <></>;
    }

    const action = determineGameAction(currentCursorPos.x, currentCursorPos.y, draggedCard);
    const indicatorContent = getIndicatorContent(action.moveType);

    const isDraggedCardSelected = selectedCards.some(selectedCard =>
        selectedCard.value === draggedCard.value && selectedCard.suit === draggedCard.suit
    );
    const cardsToUse = isDraggedCardSelected && selectedCards.length > 0 ? selectedCards : [draggedCard];

    return (
        <div className="drag-shadow" style={{ left: currentCursorPos.x, top: currentCursorPos.y - 40 }}>
            {indicatorContent && (
                <div className="drag-shadow__indicator">{indicatorContent}</div>
            )}
            <div className="drag-shadow__cards">
                {cardsToUse.map((card, index) => 
                    <CardFace 
                        card={card}
                        key={`shadow-${card.value}-${card.suit}-${index}`}
                        style={{
                            transform: 'scale(0.8)',
                            transformOrigin: 'center',
                            marginRight: '2px',
                            boxShadow: '0 2px 4px rgba(0,0,0,0.3)',
                            opacity: 0.9,
                        }}
                    />
                )}
            </div>
        </div>
    );
};
