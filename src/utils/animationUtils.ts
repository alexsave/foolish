import { tableCards, type TableView, type ViewCard as Card } from "../state/view";

export const getTableCards = (view: TableView): Card[] => tableCards(view);


export const cardsIntersection = (arr1: readonly Card[], arr2: readonly Card[]): Card[] => arr1.filter(card => arr2.some(c => c.suit === card.suit && c.value === card.value));

export const getCardKeyPlayerId = (card: Card, playerId?: string) => `${card.suit}-${card.value}-${playerId || 'global'}`;

export const getCardKey = (card: Card) => `${card.suit}-${card.value}`;

export const createCardEventString = (
    type: string,
    card: Card,
    fromLocation: string,
    toLocation: string,
    playerId?: string
): string => {
    return JSON.stringify({
        type,
        card,
        from_location: fromLocation,
        to_location: toLocation,
        player_id: playerId
    });
};