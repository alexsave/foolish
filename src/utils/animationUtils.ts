import { Battle, Card, PublicGame } from "@shared/core/types.ts";

export const getTableCards = (gameState: PublicGame): Card[] => gameState.table_battles
    ?.flatMap((b: Battle) => b.defense ? [b.attack, b.defense] : [b.attack]) || [];


export const cardsIntersection = (arr1: Card[], arr2: Card[]): Card[] => arr1.filter(card => arr2.some(c => c.suit === card.suit && c.value === card.value));

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