import { Card, PersonalGame, PublicPlayer } from '@shared/types.ts';
import { canCover, get_next_player_index } from '@shared/common_utils.ts';

// Boolean validation functions for UI (buttons/drag) - return true if valid
export const canAttack = (game: PersonalGame, cards: Card[]): boolean => {
    if (cards.length === 0) return false;

    // Get the defender's hand size
    const defenderHandSize = game.players[game.defender]?.hand_length || 0;
    
    // Calculate total UNCOVERED cards that would be on the table after this attack
    // Only count uncovered battles because covered battles are waiting for "good"
    const uncoveredBattles = game.table_battles.filter(battle => !battle.defense).length;
    const totalAfterAttack = uncoveredBattles + cards.length;
    
    // Cannot attack with more cards than defender can handle
    if (totalAfterAttack > defenderHandSize) {
        return false;
    }

    if (game.table_battles.length === 0) {
        // First attack: all cards must have the same value
        return cards.every(card => card.value === cards[0].value);
    } else {
        // Subsequent attack: all card values must already be on the table
        const tableValues = new Set(game.table_battles.flatMap(battle => 
            [battle.attack.value, ...(battle.defense ? [battle.defense.value] : [])]
        ));
        
        return cards.every(card => tableValues.has(card.value));
    }
};

export const canPass = (game: PersonalGame, cards: Card[]): boolean => {
    if (cards.length === 0 || game.table_battles.length === 0) return false;

    // All selected cards must have the same value
    if (!cards.every(card => card.value === cards[0].value)) {
        return false;
    }

    // All table battles must be uncovered and have the same value as the selected cards
    const allUncoveredWithSameValue = game.table_battles.every(battle =>
        battle.defense === null && battle.attack.value === cards[0].value
    );
    
    if (!allUncoveredWithSameValue) {
        return false;
    }

    // Find the next player (clockwise from defender). Must skip ELIMINATED
    // players exactly like the server (get_next_player_index) and the bot's
    // legal-move enumeration do — otherwise, when the seat immediately after the
    // defender is out, this looks at an out player's empty hand and wrongly hides
    // a pass the server would accept.
    const nextPlayerIndex = get_next_player_index(game, game.defender);
    const nextPlayerHandSize = game.players[nextPlayerIndex]?.hand_length || 0;
    
    // Calculate total cards that would be passed (only uncovered battles get passed)
    // Note: The check above already ensures all battles are uncovered for passing,
    // but we count explicitly for clarity and future-proofing
    const uncoveredBattles = game.table_battles.filter(battle => !battle.defense).length;
    const totalCardsAfterPass = uncoveredBattles + cards.length;
    
    // Cannot pass if next player doesn't have enough cards to defend
    return totalCardsAfterPass <= nextPlayerHandSize;
};

export const canCoverCards = (game: PersonalGame, selectedCards: Card[]): boolean => {
    if (selectedCards.length === 0) return false;

    const uncoveredBattles = game.table_battles.filter(battle => !battle.defense);
    if (uncoveredBattles.length === 0) return false;

    // For single card cover - only allow if it can cover EXACTLY one attack (unambiguous)
    if (selectedCards.length === 1) {
        const validTargets = uncoveredBattles.filter(battle => 
            canCover(battle.attack, selectedCards[0], game.power_suit)
        );
        // Only show button if there's exactly one valid target
        return validTargets.length === 1;
    }

    // For multi-card cover, check if the mapping is unambiguous
    // (all valid combinations cover the same set of attacks)
    const findCoverCombinations = (coverCards: Card[], uncoveredAttacks: Card[]): { coverCards: Card[], attackCards: Card[] }[] => {
        const combinations: { coverCards: Card[], attackCards: Card[] }[] = [];
        
        if (coverCards.length > uncoveredAttacks.length) return combinations;

        const generatePermutations = (arr: Card[], length: number): Card[][] => {
            if (length === 1) return arr.map(item => [item]);
            
            const result: Card[][] = [];
            for (let i = 0; i < arr.length; i++) {
                const rest = arr.slice(0, i).concat(arr.slice(i + 1));
                const subPermutations = generatePermutations(rest, length - 1);
                for (const subPerm of subPermutations) {
                    result.push([arr[i], ...subPerm]);
                }
            }
            return result;
        };

        const attackPermutations = generatePermutations(uncoveredAttacks, coverCards.length);
        
        for (const attackPerm of attackPermutations) {
            const isValidCombination = coverCards.every((coverCard, index) => 
                canCover(attackPerm[index], coverCard, game.power_suit)
            );
            
            if (isValidCombination) {
                combinations.push({
                    coverCards: [...coverCards],
                    attackCards: [...attackPerm]
                });
            }
        }

        return combinations;
    };

    const uncoveredAttacks = uncoveredBattles.map(battle => battle.attack);
    const validCombinations = findCoverCombinations(selectedCards, uncoveredAttacks);
    
    if (validCombinations.length === 0) {
        return false;
    }
    
    // Check if all valid combinations result in the same set of attack cards being covered
    const cardToString = (card: Card) => `${card.value}-${card.suit}`;
    const firstCombinationAttackSet = new Set(validCombinations[0].attackCards.map(cardToString));
    
    const allCombinationsHaveSameAttackSet = validCombinations.every(combo => {
        const comboAttackSet = new Set(combo.attackCards.map(cardToString));
        return comboAttackSet.size === firstCombinationAttackSet.size && 
               Array.from(comboAttackSet).every(cardStr => firstCombinationAttackSet.has(cardStr));
    });
    
    // Only show button if it's unambiguous which attacks will be covered
    return allCombinationsHaveSameAttackSet;
};

// Throwing validation functions for server-side validation
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