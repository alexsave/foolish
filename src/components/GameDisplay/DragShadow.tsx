import { useDrag } from "../../contexts/DragContext";
import { useGame } from "../../contexts/GameContext";
import { CardFace } from "./CardFace";

export const DragShadow = () => {
    const { selectedCards } = useGame();
    const { isDraggingForGameAction, draggedCard, currentCursorPos, determineGameAction } = useDrag();

    const dragStyle = {
        border: '2px solid black',
        backgroundColor: 'white',
        width: '36px', // Updated to 5:7 ratio (36:50, rounded from 35.7)
        height: '50px',
        borderRadius: '4px',
        display: 'flex',
        justifyContent: 'center',
        alignItems: 'center',
        marginRight: '2px',
        boxShadow: '0 2px 4px rgba(0,0,0,0.3)',
        opacity: 0.9,
        fontSize: '12px',
    }

    const getIndicatorText = (actionType: string) => {
        switch (actionType) {
            case 'attack':
                return `⚔️ Attack`;
            case 'cover':
            case 'multicover':
                return '🛡️ Cover';
            case 'pass':
                return `🔄 Pass`;
            default:
                return null; // No indicator for invalid actions
        }
    }

    // Always show the card when dragging for game action, even if no valid action
    if (!isDraggingForGameAction || !draggedCard || !currentCursorPos) {
        return <></>;
    }

    const action = determineGameAction(currentCursorPos.x, currentCursorPos.y, draggedCard);
    const indicatorText = getIndicatorText(action.type);

    const isDraggedCardSelected = selectedCards.some(selectedCard =>
        selectedCard.value === draggedCard.value && selectedCard.suit === draggedCard.suit
    );
    const cardsToUse = isDraggedCardSelected && selectedCards.length > 0 ? selectedCards : [draggedCard];

    return <div style={{
        position: 'absolute',
        left: currentCursorPos.x,
        top: currentCursorPos.y - 40,
        transform: 'translate(-50%, -50%)',
        display: 'flex',
        flexDirection: 'column',
        justifyContent: 'center',
        alignItems: 'center',
        zIndex: 2001,
    }}>
        {/* Only show action indicator when there's a valid action */}
        {indicatorText && (
            <div style={{
                backgroundColor: 'rgba(0, 0, 0, 0.8)',
                color: 'white',
                padding: '4px 8px',
                borderRadius: '4px',
                fontSize: '12px',
                pointerEvents: 'none'
            }}>
                {indicatorText}
            </div>
        )}
        {/* Always show shadow cards when dragging outside hand */}
        <div>{
            cardsToUse.map((card, index) => 
                // Maybe a scale down is all we need?
                <CardFace 
                    card={card}
                    key={`shadow-${card.value}-${card.suit}-${index}`}
                    style={dragStyle}
                />
            )
        }</div>
    </div>
}
