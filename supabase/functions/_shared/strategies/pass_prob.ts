/**
 * Pass-back probability calculations for Durak (perevodnoy/transfer logic)
 * 
 * Uses inclusion-exclusion to calculate the probability that a chain of
 * passers can each contribute at least one card of the attacked rank.
 * 
 * Constraint: a + d <= 4 (attack size + distance must not exceed 4 copies per rank)
 */

import { Game, Card } from '../types.ts';
import { CardTracker } from '../durakai/cardTracker.ts';

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/** Convert card to string key */
function cardKey(card: Card): string {
    return `${card.suit}-${card.value}`;
}

/**
 * Calculate combination C(n, k) = n! / (k! * (n-k)!)
 * Returns 0 if n < k or n < 0 or k < 0
 */
function combination(n: number, k: number): number {
    if (k < 0 || n < 0 || n < k) return 0;
    if (k === 0 || k === n) return 1;
    
    // Use the smaller of k and n-k for efficiency
    if (k > n - k) k = n - k;
    
    let result = 1;
    for (let i = 0; i < k; i++) {
        result = result * (n - i) / (i + 1);
    }
    return Math.round(result);
}

/** Count set bits in a number */
function countBits(n: number): number {
    let count = 0;
    while (n) {
        count += n & 1;
        n >>= 1;
    }
    return count;
}

// ============================================================================
// QUICK CHECKS
// ============================================================================

/**
 * Check if pass-back is definitely impossible
 * Returns true if a + d > 4 (can't have enough cards of the rank)
 * 
 * @param attackSize - number of cards of rank v already on the table
 * @param distance - number of passers in the chain
 */
export function isPassDefinitelyImpossible(attackSize: number, distance: number): boolean {
    return attackSize + distance > 4;
}


// ============================================================================
// TRACKER-AWARE PRE-CHECKS
// ============================================================================

export interface PassPreCheckResult {
    isDefinitelyPossible: boolean;
    isDefinitelyImpossible: boolean;
    reason: string;
    knownPasserHasRank: boolean[];  // Which passers we KNOW have the rank
    remainingCopies: number;        // R = 4 - a
    copiesInPasserHands: number;    // How many of R we KNOW are in passer hands
    copiesAccountedElsewhere: number; // How many of R are in discard/my hand/flipped/other players
    copiesUnknown: number;          // How many copies are in the unknown pool
}

/**
 * Pre-check pass possibility using CardTracker
 * 
 * This determines:
 * 1. DEFINITE POSSIBLE: We KNOW all passers have the needed card
 * 2. DEFINITE IMPOSSIBLE: 
 *    - a + d > 4 (structural impossibility)
 *    - All R remaining copies are accounted for and NONE are in any passer's hand
 * 3. UNCERTAIN: Need to calculate probability
 * 
 * @param game - Game state
 * @param tracker - CardTracker with known card locations
 * @param attackValue - The rank being attacked (value)
 * @param attackSize - Number of cards of this rank on the table (a)
 * @param passerIds - Player IDs of passers in order (defender first, then clockwise to attacker)
 * @param myPlayerId - The player making this calculation
 */
export function preCheckPassWithTracker(
    game: Game,
    tracker: CardTracker,
    attackValue: number,
    attackSize: number,
    passerIds: string[],
    myPlayerId: string
): PassPreCheckResult {
    const distance = passerIds.length;
    const R = 4 - attackSize; // Remaining copies that could enable passing
    
    // Quick structural check
    if (isPassDefinitelyImpossible(attackSize, distance)) {
        return {
            isDefinitelyPossible: false,
            isDefinitelyImpossible: true,
            reason: `a + d = ${attackSize} + ${distance} > 4 (not enough copies exist)`,
            knownPasserHasRank: [],
            remainingCopies: R,
            copiesInPasserHands: 0,
            copiesAccountedElsewhere: 0,
            copiesUnknown: 0
        };
    }
    
    // Find all 4 copies of this rank and where they are
    const allCopies: { suit: number; location: string }[] = [];
    for (let suit = 0; suit < 4; suit++) {
        const key = cardKey({ suit, value: attackValue });
        
        // Check each possible location
        let location = 'unknown';
        
        // Is it on the table (part of the attack or defense)?
        const onTable = game.table_battles.some(battle => 
            (battle.attack && battle.attack.suit === suit && battle.attack.value === attackValue) ||
            (battle.defense && battle.defense.suit === suit && battle.defense.value === attackValue)
        );
        if (onTable) {
            location = 'table';
        }
        // Is it in my hand?
        else {
            const myPlayer = game.players.find(p => p.player_id === myPlayerId);
            if (myPlayer?.hand.some(c => c.suit === suit && c.value === attackValue)) {
                location = 'my_hand';
            }
            // Is it the flipped card?
            else if (game.flipped && game.flipped.suit === suit && game.flipped.value === attackValue) {
                location = 'flipped';
            }
            // Is it in discard?
            else if (tracker.getCardsInDiscard().some(c => c.suit === suit && c.value === attackValue)) {
                location = 'discard';
            }
            // Is it known to be in a passer's hand?
            else {
                for (const passerId of passerIds) {
                    const knownCards = tracker.knownCardsByPlayer.get(passerId);
                    if (knownCards?.has(key)) {
                        location = `passer:${passerId}`;
                        break;
                    }
                }
                // Is it known to be in another player's hand (not a passer)?
                if (location === 'unknown') {
                    for (const [playerId, knownCards] of tracker.knownCardsByPlayer) {
                        if (!passerIds.includes(playerId) && knownCards.has(key)) {
                            location = `other:${playerId}`;
                            break;
                        }
                    }
                }
            }
        }
        
        allCopies.push({ suit, location });
    }
    
    // Count copies by location type
    const copiesOnTable = allCopies.filter(c => c.location === 'table').length;
    const copiesInMyHand = allCopies.filter(c => c.location === 'my_hand').length;
    const copiesFlipped = allCopies.filter(c => c.location === 'flipped').length;
    const copiesInDiscard = allCopies.filter(c => c.location === 'discard').length;
    const copiesInPasserHands = allCopies.filter(c => c.location.startsWith('passer:')).length;
    const copiesInOtherHands = allCopies.filter(c => c.location.startsWith('other:')).length;
    const copiesUnknown = allCopies.filter(c => c.location === 'unknown').length;
    
    // The attack already accounts for `attackSize` copies (on table)
    // R = 4 - attackSize is what's left
    // But some of those R might be accounted for elsewhere
    const copiesAccountedElsewhere = copiesInMyHand + copiesFlipped + copiesInDiscard + copiesInOtherHands;
    
    // Check which passers we KNOW have the rank
    const knownPasserHasRank: boolean[] = passerIds.map(passerId => {
        const knownCards = tracker.knownCardsByPlayer.get(passerId);
        if (!knownCards) return false;
        for (const cardKeyStr of knownCards) {
            const [, cardValue] = cardKeyStr.split('-').map(Number);
            if (cardValue === attackValue) return true;
        }
        return false;
    });
    
    // DEFINITE POSSIBLE: All passers known to have the rank
    if (knownPasserHasRank.every(has => has)) {
        return {
            isDefinitelyPossible: true,
            isDefinitelyImpossible: false,
            reason: `All ${distance} passer(s) known to have rank ${attackValue}`,
            knownPasserHasRank,
            remainingCopies: R,
            copiesInPasserHands,
            copiesAccountedElsewhere,
            copiesUnknown
        };
    }
    
    // DEFINITE IMPOSSIBLE: All R copies are accounted for and none are in passer hands
    // (and none are unknown)
    if (copiesUnknown === 0 && copiesInPasserHands === 0) {
        return {
            isDefinitelyPossible: false,
            isDefinitelyImpossible: true,
            reason: `All copies of rank ${attackValue} accounted for, none in any passer's hand`,
            knownPasserHasRank,
            remainingCopies: R,
            copiesInPasserHands,
            copiesAccountedElsewhere,
            copiesUnknown
        };
    }
    
    // DEFINITE IMPOSSIBLE: Not enough unknown + passer copies to give each passer one
    // Each passer needs at least 1 copy. If copiesUnknown + copiesInPasserHands < distance, impossible
    const availableForPassers = copiesUnknown + copiesInPasserHands;
    const passersNeedingCopies = knownPasserHasRank.filter(has => !has).length;
    if (availableForPassers < passersNeedingCopies) {
        return {
            isDefinitelyPossible: false,
            isDefinitelyImpossible: true,
            reason: `Only ${availableForPassers} copies available but ${passersNeedingCopies} passers need one`,
            knownPasserHasRank,
            remainingCopies: R,
            copiesInPasserHands,
            copiesAccountedElsewhere,
            copiesUnknown
        };
    }
    
    // UNCERTAIN: Need to calculate probability
    return {
        isDefinitelyPossible: false,
        isDefinitelyImpossible: false,
        reason: `${copiesUnknown} unknown copies, ${copiesInPasserHands} known in passer hands`,
        knownPasserHasRank,
        remainingCopies: R,
        copiesInPasserHands,
        copiesAccountedElsewhere,
        copiesUnknown
    };
}

// ============================================================================
// GENERAL FORMULA (UNEQUAL HAND SIZES)
// ============================================================================

/**
 * Calculate pass-back probability using inclusion-exclusion
 * 
 * Formula:
 *   P(d,a) = (1/C(N,R)) * Σ_{J ⊆ {1,...,d}} (-1)^|J| * C(N - Σ_{j∈J} h_j, R)
 * 
 * where R = 4 - a (remaining copies of rank v in unknown world), unless overridden by effectiveR
 * 
 * @param d - distance (number of passers): 1, 2, or 3
 * @param a - attack size (cards of rank v on table/being played): 1, 2, or 3
 * @param N - total unknown cards
 * @param handSizes - array of hand sizes for each passer [h1, h2, ...] (length >= d)
 * @param effectiveR - optional override for R (useful when some copies are in our hand but not attacked with)
 * @returns probability that all passers can pass (each has at least one card of rank v)
 */
export function calculatePassProbability(
    d: number,
    a: number,
    N: number,
    handSizes: number[],
    effectiveR?: number
): number {
    // Quick checks
    if (d <= 0) return 0;    // No passers
    if (N <= 0) return 0;    // No unknown cards
    
    // Use effectiveR if provided, otherwise compute from attack size
    const R = effectiveR ?? (4 - a);
    
    // Check if pass is structurally possible - need at least d copies for d passers
    if (R < d) return 0;
    if (R <= 0) return 0;
    
    const denominator = combination(N, R);
    if (denominator === 0) return 0;
    
    // Inclusion-exclusion over all subsets J of {1, ..., d}
    let numerator = 0;
    
    // Iterate over all 2^d subsets using bitmask
    for (let mask = 0; mask < (1 << d); mask++) {
        const subsetSize = countBits(mask);
        const sign = (subsetSize % 2 === 0) ? 1 : -1;
        
        // Sum of hand sizes for players in subset J
        let sumH = 0;
        for (let j = 0; j < d; j++) {
            if ((mask >> j) & 1) {
                sumH += handSizes[j] ?? 6; // Default to 6 if not specified
            }
        }
        
        numerator += sign * combination(N - sumH, R);
    }
    
    return Math.max(0, Math.min(1, numerator / denominator));
}
