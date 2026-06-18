import { Card, Battle } from '@shared/types.ts';
import { canCover } from '@shared/common_utils.ts';

// Shared multi-card cover resolution, used by both the drag (DragContext) and
// keyboard (KeyboardInputHandler) input paths to decide whether a set of
// selected cards covers the uncovered attacks in exactly one unambiguous way.

export interface CoverCombination {
    coverCards: Card[];
    attackCards: Card[];
}

// All valid ways to pair every cover card with a distinct uncovered attack.
export const findCoverCombinations = (
    coverCards: Card[],
    uncoveredAttacks: Card[],
    powerSuit: number
): CoverCombination[] => {
    const combinations: CoverCombination[] = [];

    if (coverCards.length === 0 || uncoveredAttacks.length === 0) {
        return combinations;
    }

    // For each permutation of uncovered attacks (taking coverCards.length items)
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

    // Only consider combinations where we have the right number of cards
    if (coverCards.length <= uncoveredAttacks.length) {
        const attackPermutations = generatePermutations(uncoveredAttacks, coverCards.length);

        for (const attackPerm of attackPermutations) {
            // Check if this cover-attack pairing is valid
            const isValidCombination = coverCards.every((coverCard, index) =>
                canCover(attackPerm[index], coverCard, powerSuit)
            );

            if (isValidCombination) {
                combinations.push({
                    coverCards: [...coverCards],
                    attackCards: [...attackPerm]
                });
            }
        }
    }

    return combinations;
};

// Returns a cover combination only if every valid pairing covers the same set
// of attacks (i.e. the cover is unambiguous); otherwise null.
export const findUnambiguousCover = (
    cardsToUse: Card[],
    tableBattles: Battle[],
    powerSuit: number
): CoverCombination | null => {
    const uncoveredAttacks = tableBattles
        .filter(battle => !battle.defense)
        .map(battle => battle.attack);

    const validCombinations = findCoverCombinations(cardsToUse, uncoveredAttacks, powerSuit);

    if (validCombinations.length === 0) {
        return null;
    }

    // Check if all valid combinations result in the same set of attack cards being covered
    const cardToString = (card: Card) => `${card.value}-${card.suit}`;
    const firstCombinationAttackSet = new Set(validCombinations[0].attackCards.map(cardToString));

    const allCombinationsHaveSameAttackSet = validCombinations.every(combo => {
        const comboAttackSet = new Set(combo.attackCards.map(cardToString));
        return comboAttackSet.size === firstCombinationAttackSet.size &&
               Array.from(comboAttackSet).every(cardStr => firstCombinationAttackSet.has(cardStr));
    });

    // If all combinations cover the same set of attack cards, it's unambiguous
    if (allCombinationsHaveSameAttackSet) {
        return validCombinations[0]; // Return any valid combination since they all cover the same attacks
    }

    return null;
};
