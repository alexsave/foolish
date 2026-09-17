import { tableCards, type TableView, type ViewCard as Card } from "../state/view";

export const getTableCards = (view: TableView): Card[] => tableCards(view);


export const cardsIntersection = (arr1: readonly Card[], arr2: readonly Card[]): Card[] => arr1.filter(card => arr2.some(c => c.suit === card.suit && c.value === card.value));

// A card's animation key: the card and whose it is - a seat, or a place's own key ('table', 'flipped').
export const getCardKeyOwner = (card: Card, owner?: number | string) => `${card.suit}-${card.value}-${owner ?? 'global'}`;

export const getCardKey = (card: Card) => `${card.suit}-${card.value}`;

export const createCardEventString = (
    type: string,
    card: Card,
    fromLocation: string,
    toLocation: string,
    seat?: number
): string => {
    return JSON.stringify({
        type,
        card,
        from_location: fromLocation,
        to_location: toLocation,
        seat
    });
};