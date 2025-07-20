import { Card, PersonalGame, PublicPlayer } from '../common/types';
import { get_next_player_index, canCover } from '../common/common_utils';

export const validateAttack = (game: PersonalGame, cards: Card[]): void => {
    const table_battles = game.table_battles;
    const uncovered_cards = table_battles.filter(battle => battle.defense === null).length;
    const defender: PublicPlayer = game.players[game.defender];
    const defender_cards = defender.hand_length;

    if (uncovered_cards + cards.length > defender_cards) {
        throw new Error('No room in defenders hand');
    }
    
    if (table_battles.length > 0 && !cards.every(card => table_battles.some(battle => battle.attack.value === card.value || battle.defense?.value === card.value))) {
        throw new Error('Some card values are not on the table');
    }
};

export const validatePass = (game: PersonalGame, cards: Card[]): void => {
    const table_battles = game.table_battles;
    
    if (!cards.every(card => card.value === cards[0].value)) {
        throw new Error('Some card values are not the same');
    }
    
    if (!table_battles.every(battle => battle.defense === null && battle.attack.value === cards[0].value)) {
        throw new Error('Cannot pass');
    }
};

export const validatePickup = (game: PersonalGame): void => {
    const table_battles = game.table_battles;
    
    if (table_battles.length === 0) {
        throw new Error('Cannot pickup');
    }
};

export const validateCover = (game: PersonalGame, coverCards: Card[], attackCards: Card[]): void => {
    const table_battles = game.table_battles;
    
    if (table_battles.length === 0) {
        throw new Error('Cannot cover');
    }
    
    for (let i = 0; i < coverCards.length; i++) {
        const coverCard = coverCards[i];
        const attackCard = attackCards[i];
        if (!canCover(attackCard, coverCard, game.power_suit)) {
            throw new Error('Cover card value does not match attack card value');
        }
    }
}; 