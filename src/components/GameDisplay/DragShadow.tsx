import { PersonalGame } from "../../common/types";
import { Card } from "../../common/types";
import { VALUE_MAP, SUIT_MAP } from "../../utils/cards";

export const DragShadow = ({ state, isDraggingForGameAction, draggedCard, currentCursorPos, selectedCards, determineGameAction, user_id }: { state: PersonalGame, isDraggingForGameAction: boolean, draggedCard: Card | null, currentCursorPos: { x: number, y: number } | null, selectedCards: Card[], determineGameAction: (x: number, y: number, draggedCard: Card) => { type: string }, user_id: string | null }) => {
    return <>
        {/* Floating action indicator during game action drag */}
        {isDraggingForGameAction && draggedCard && currentCursorPos && (() => {
            const action = determineGameAction(currentCursorPos.x, currentCursorPos.y, draggedCard);
            return action.type === 'attack' || action.type === 'cover' || action.type === 'pass';
        })() && (
                <div style={{
                    position: 'absolute',
                    left: currentCursorPos.x - 20,
                    top: currentCursorPos.y + 10,
                    backgroundColor: 'rgba(0, 0, 0, 0.8)',
                    color: 'white',
                    padding: '4px 8px',
                    borderRadius: '4px',
                    fontSize: '12px',
                    zIndex: 1001,
                    pointerEvents: 'none'
                }}>
                    {(() => {
                        const self_index = state.players.findIndex((player) => player.player_id === user_id);
                        const isDefending = state.defender === self_index;
                        const action = determineGameAction(currentCursorPos.x, currentCursorPos.y, draggedCard);

                        // Check if the dragged card is part of selected cards
                        const isDraggedCardSelected = selectedCards.some(selectedCard =>
                            selectedCard.value === draggedCard.value && selectedCard.suit === draggedCard.suit
                        );

                        // Use all selected cards if the dragged card is selected, otherwise just the dragged card
                        const cardsToUse = isDraggedCardSelected && selectedCards.length > 0 ? selectedCards : [draggedCard];
                        const cardCount = cardsToUse.length;
                        const cardCountText = '';//cardCount > 1 ? ` (${cardCount})` : '';

                        switch (action.type) {
                            case 'attack':
                                return `⚔️ Attack${cardCountText}`;
                            case 'cover':
                                return '🛡️ Cover';
                            case 'pass':
                                return `🔄 Pass${cardCountText}`;
                            default:
                                return '❓';
                        }
                    })()}
                </div>
            )}

        {/* Shadow cards showing what's being dragged */}
        {isDraggingForGameAction && draggedCard && currentCursorPos && (() => {
            const action = determineGameAction(currentCursorPos.x, currentCursorPos.y, draggedCard);
            return action.type === 'attack' || action.type === 'cover' || action.type === 'pass';
        })() && (
                <div style={{
                    position: 'absolute',
                    left: currentCursorPos.x - 10,
                    top: currentCursorPos.y - 50,
                    display: 'flex',
                    flexDirection: 'row',
                    alignItems: 'center',
                    zIndex: 1002,
                    pointerEvents: 'none'
                }}>
                    {(() => {
                        // Check if the dragged card is part of selected cards
                        const isDraggedCardSelected = selectedCards.some(selectedCard =>
                            selectedCard.value === draggedCard.value && selectedCard.suit === draggedCard.suit
                        );

                        // Use all selected cards if the dragged card is selected, otherwise just the dragged card
                        const cardsToUse = isDraggedCardSelected && selectedCards.length > 0 ? selectedCards : [draggedCard];

                        return cardsToUse.map((card, index) => (
                            <div
                                key={`shadow-${card.value}-${card.suit}-${index}`}
                                style={{
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
                                }}
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
                            </div>
                        ));
                    })()}
                </div>
            )}
    </>
}