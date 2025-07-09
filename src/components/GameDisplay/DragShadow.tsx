import { VALUE_MAP, SUIT_MAP } from "../../utils/cards";
import { useDrag } from "../../contexts/DragContext";
import { useGame } from "../../contexts/GameContext";

export const DragShadow = () => {
    const { selectedCards } = useGame();
    const { isDraggingForGameAction, draggedCard, currentCursorPos, determineGameAction } = useDrag();

    const dragStyle = {
        border: '2px solid black',
        backgroundColor: 'white',
        width: '30px',
        height: '50px',
        borderRadius: '4px',
        display: 'flex',
        justifyContent: 'center',
        alignItems: 'center',
        marginRight: '2px',
        boxShadow: '0 2px 4px rgba(0,0,0,0.3)',
        opacity: 0.9
    }

    const getIndicatorText = () => {
        switch (action.type) {
            case 'attack':
                return `⚔️ Attack`;
            case 'cover':
                return '🛡️ Cover';
            case 'pass':
                return `🔄 Pass`;
            default:
                return '❓';
        }
    }

    if (!isDraggingForGameAction || !draggedCard || !currentCursorPos) {
        return <></>;
    }

    const action = determineGameAction(currentCursorPos.x, currentCursorPos.y, draggedCard);

    if (!['attack', 'cover', 'pass'].includes(action.type)) {
        return <></>;
    }

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
        {/* Floating action indicator during game action drag */}
        <div style={{
            backgroundColor: 'rgba(0, 0, 0, 0.8)',
            color: 'white',
            padding: '4px 8px',
            borderRadius: '4px',
            fontSize: '12px',
            pointerEvents: 'none'
        }}>
            {getIndicatorText()}
        </div>
        {/* Shadow cards showing what's being dragged */}
        <div>{
            cardsToUse.map((card, index) => <div
                key={`shadow-${card.value}-${card.suit}-${index}`}
                style={dragStyle}
            >
                <p style={{
                    pointerEvents: 'none',
                    userSelect: 'none',
                    textAlign: 'center',
                    fontSize: '12px',
                    margin: 0
                }}>
                    {VALUE_MAP[card.value]}
                    <br />
                    {SUIT_MAP[card.suit]}
                </p>
            </div>)
        }</div>
    </div>
}