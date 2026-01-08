import { Game, Card } from '../types.ts';
import { LegalMove } from '../bot_interfaces.ts';
import { CardTracker } from '../durakai/cardTracker.ts';
import { cardDisplay, getCardValue, get_next_player_index, canCover } from '../common_utils.ts';
import { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../types.ts';
import { SUITS } from '../constants.ts';
import { 
    preCheckPassWithTracker, 
    calculatePassProbability as calcPassProbFormula,
    isPassDefinitelyImpossible 
} from './pass_prob.ts';

// Value names for debugging (value 5 = "6", value 6 = "7", etc.)
const VALUE_NAMES: Record<number, string> = {
    5: '6', 6: '7', 7: '8', 8: '9', 9: '10', 10: 'J', 11: 'Q', 12: 'K', 13: 'A'
};

export interface AttackStats {
    canDefinitelyCover: boolean;
    definitelyCannotCover: boolean;
    canDefinitelyPassBack: boolean;
    definitelyCannotPassBack: boolean;
    probCover: number;
    probPass: number;
    probCoverAllowsAttack: number;
}

export interface CoverStats {
    probAllowsAdditionalAttack: number;
    probAllowsUncoverableAttack: number;
    probDrawBetterCard: number;
}

export interface PassStats {
    canDefinitelyCover: boolean;
    definitelyCannotCover: boolean;
    canDefinitelyPassBack: boolean;
    definitelyCannotPassBack: boolean;
    probCover: number;
    probPass: number;
    probCoverAllowsAttack: number;
}

export interface MoveStats {
    attack?: AttackStats;
    cover?: CoverStats;
    pass?: PassStats;
}

/**
 * Calculate statistics for a legal move
 */
export function calculateMoveStats(
    game: Game,
    playerId: string,
    move: LegalMove,
    tracker: CardTracker
): { stats: MoveStats | null; debug: DebugInfo | null } {
    switch (move.type) {
        case 'attack': {
            const { stats, debug } = calculateAttackStatsWithDebug(game, playerId, move, tracker);
            return { stats: { attack: stats }, debug: { attack: debug } };
        }
        case 'cover': {
            const { stats, debug } = calculateCoverStatsWithDebug(game, playerId, move, tracker);
            return { stats: { cover: stats }, debug: { cover: debug } };
        }
        case 'pass': {
            const { stats, debug } = calculatePassStatsWithDebug(game, playerId, move, tracker);
            return { stats: { pass: stats }, debug: { pass: debug } };
        }
        default:
            return { stats: null, debug: null };
    }
}

/**
 * Format move stats as a single line summary
 * Uses DP algorithms for P(Cover) and inclusion-exclusion for P(PassBack)
 */
export function formatMoveStats(stats: MoveStats | null, _debugInfo?: DebugInfo): string {
    if (!stats) return '';
    
    if (stats.attack) {
        const a = stats.attack;
        // Attack: P(Cover), P(CoverAllowsAtk), P(PassBack)
        return `      P(Cover): ${pctDef(a.probCover, a.canDefinitelyCover, a.definitelyCannotCover)} | P(CoverAllowsAtk): ${pct(a.probCoverAllowsAttack)} | P(PassBack): ${pctDef(a.probPass, a.canDefinitelyPassBack, a.definitelyCannotPassBack)}`;
    }
    
    if (stats.cover) {
        const c = stats.cover;
        // Cover: P(AllowsAtk), P(ForcesPickup), P(DrawBetter)
        return `      P(AllowsAtk): ${pct(c.probAllowsAdditionalAttack)} | P(ForcesPickup): ${pct(c.probAllowsUncoverableAttack)} | P(DrawBetter): ${pct(c.probDrawBetterCard)}`;
    }
    
    if (stats.pass) {
        const p = stats.pass;
        // Pass: P(Cover) for new defender, P(CoverAllowsAtk), P(PassBack) to us
        return `      P(Cover): ${pctDef(p.probCover, p.canDefinitelyCover, p.definitelyCannotCover)} | P(CoverAllowsAtk): ${pct(p.probCoverAllowsAttack)} | P(PassBack): ${pctDef(p.probPass, p.canDefinitelyPassBack, p.definitelyCannotPassBack)}`;
    }
    
    return '';
}

function pctDef(p: number, definitely: boolean, definitelyNot: boolean): string {
    if (definitely || p >= 0.9999) return '100.00%✓';
    if (definitelyNot || p <= 0.0001) return '0.00%✗';
    return `${(p * 100).toFixed(2)}%`;
}

function pct(p: number): string {
    return `${(p * 100).toFixed(2)}%`;
}

/**
 * Compute combination C(n,k) = n choose k
 * Returns 0 if k > n or either is negative
 */
function combination(n: number, k: number): number {
    if (k > n || k < 0 || n < 0) return 0;
    if (k === 0 || k === n) return 1;
    
    // Use smaller k for efficiency: C(n,k) = C(n, n-k)
    if (k > n - k) k = n - k;
    
    // C(n,k) = (n/1) * ((n-1)/2) * ... * ((n-k+1)/k)
    let result = 1;
    for (let i = 0; i < k; i++) {
        result = result * (n - i) / (i + 1);
    }
    return Math.round(result);
}

/**
 * Generate formula string for combination: (n/1)×((n-1)/2)×...
 */
function combinationFormula(n: number, k: number): string {
    if (k > n || k < 0 || n < 0) return '0';
    if (k === 0) return '1';
    if (k === n) return '1';
    
    // Use smaller k for efficiency
    const origK = k;
    if (k > n - k) k = n - k;
    
    const parts: string[] = [];
    for (let i = 0; i < k; i++) {
        parts.push(`(${n - i}/${i + 1})`);
    }
    return parts.join('×');
}

// Debug info interfaces for formula display
export interface DebugInfo {
    attack?: AttackDebugInfo;
    cover?: CoverDebugInfo;
    pass?: PassDebugInfo;
}

export interface AttackDebugInfo {
    unknownTotal: number;
    coverCardsCount: number;
    defenderUnknownHand: number;
    playersInPassChain: number;
    attackValue: number;
    valueCardsRemaining: number;
    playerUnknownHand: number;
    coversMatchingMyHand: number;
    totalPossibleCovers: number;
}

export interface CoverDebugInfo {
    unknownTotal: number;
    coverValue: number;
    valueCardsRemaining: number;
    attackerUnknownHand: number;
    canCoverValues: string;
    avgUnknownValue: number;
    cardValue: number;
}

export interface PassDebugInfo {
    unknownTotal: number;
    coverCardsCount: number;
    newDefenderUnknownHand: number;
    playersInPassChain: number;
    seatsBetween: number;
    totalAttacks: number;
    coversMatchingMyHand: number;
    totalPossibleCovers: number;
}

// ============================================================================
// DP-BASED COVER PROBABILITY (from prob.ts)
// ============================================================================

/**
 * Iterator for bits in a number
 */
function* bitsOf(x: number) {
    while (x !== 0) {
        const lsb = x & -x;
        yield lsb;
        x ^= lsb;
    }
}

const addOneCardCache = new Map<string, bigint>();

/**
 * Given a set of achievable attack subsets (A) and a card that can cover certain attacks (coverMask),
 * compute the new set of achievable subsets after adding that card.
 */
function addOneCardAchievable(A: bigint, coverMask: number, subsetCount: number): bigint {
    const key = `${A.toString()},${coverMask},${subsetCount}`;
    const cached = addOneCardCache.get(key);
    if (cached !== undefined) {
        return cached;
    }
    
    let A2 = A;
    for (let S = 0; S < subsetCount; S++) {
        const bitS = 1n << BigInt(S);
        if ((A & bitS) === 0n) continue;

        const available = coverMask & (~S);
        for (const b of bitsOf(available)) {
            const S2 = S | b;
            A2 |= 1n << BigInt(S2);
        }
    }
    addOneCardCache.set(key, A2);
    return A2;
}

/**
 * Apply a bucket of cards to the DP state.
 * @param dp - Current DP state: dp[handSize][achievableSet] = ways
 * @param bucketSize - Number of cards in this bucket
 * @param bucketMask - Bitmask indicating which attacks each card in this bucket can cover
 * @param maxHandSize - Maximum hand size (defender's hand size)
 * @param subsetCount - Total number of attack subsets (2^numAttacks)
 * @param isUseless - Whether this bucket contains useless cards (can't cover any attack)
 */
function applyBucket(
    dp: Record<number, Record<string, bigint>>,
    bucketSize: number,
    bucketMask: number,
    maxHandSize: number,
    subsetCount: number,
    isUseless = false
): Record<number, Record<string, bigint>> {
    const nextDp: Record<number, Record<string, bigint>> = {};
    
    // Memoize applyBucket for this specific bucket (only for useful buckets)
    const applyBucketMemo = isUseless ? null : new Map<string, bigint>();
    
    for (let r = 0; r <= maxHandSize; r++) {
        const row = dp[r];
        if (!row) continue;
        
        for (const [AStr, ways] of Object.entries(row)) {
            const waysAsBigInt = typeof ways === 'bigint' ? ways : BigInt(ways as string);
            
            for (let t = 0; t + r <= maxHandSize; t++) {
                const combos = combination(bucketSize, t);
                if (combos == 0) continue;
                
                let A2Str: string;
                if (isUseless) {
                    // Useless cards don't change achievability
                    A2Str = AStr;
                } else {
                    // Useful cards: apply bucket mask t times (memoized)
                    const memoKey = `${AStr}|${t}`;
                    let A2 = applyBucketMemo!.get(memoKey);
                    if (A2 === undefined) {
                        A2 = BigInt(AStr);
                        for (let i = 0; i < t; i++) {
                            A2 = addOneCardAchievable(A2, bucketMask, subsetCount);
                        }
                        applyBucketMemo!.set(memoKey, A2);
                    }
                    A2Str = A2.toString();
                }
                
                const r2 = r + t;
                if (!nextDp[r2]) nextDp[r2] = {};
                if (!nextDp[r2][A2Str]) nextDp[r2][A2Str] = 0n;
                nextDp[r2][A2Str] += waysAsBigInt * BigInt(combos);
            }
        }
    }
    return nextDp;
}

/**
 * Initialize DP state from known defender cards.
 * Since we KNOW the defender has these cards, we start at r=numKnownCards
 * with the achievability computed from ALL known cards.
 * 
 * @param knownCards - Cards we know the defender has
 * @param attacks - Attack cards
 * @param trumpSuit - Trump suit
 * @param subsetCount - Total number of attack subsets (2^numAttacks)
 * @returns Initial DP state: dp[numKnownCards][achievableSet] = 1
 */
function initializeDPWithKnownCards(
    knownCards: Card[],
    attacks: Card[],
    trumpSuit: number,
    subsetCount: number
): Record<number, Record<string, bigint>> {
    const dp: Record<number, Record<string, bigint>> = {};
    
    // Compute achievability from ALL known cards (since defender has all of them)
    let achievable = 1n << 0n; // Start with empty set achievable
    
    // For each known card, update achievability
    for (const card of knownCards) {
        // Which attacks can this card cover?
        let cardMask = 0;
        for (let i = 0; i < attacks.length; i++) {
            if (canCover(attacks[i], card, trumpSuit)) {
                cardMask |= (1 << i);
            }
        }
        
        // Update achievability using the same logic as addOneCardAchievable
        achievable = addOneCardAchievable(achievable, cardMask, subsetCount);
    }
    
    // Initialize DP at r = numKnownCards with this achievability
    const r = knownCards.length;
    dp[r] = {};
    dp[r][achievable.toString()] = 1n;
    
    return dp;
}

/**
 * Calculate the exact probability that a defender can cover all given attacks.
 * Uses dynamic programming to enumerate all possible hands and check coverability.
 * 
 * @param attacks - Array of attack cards
 * @param excludedCards - Set of card display strings for cards NOT in the unknown pool (our hand, discard, flipped, known opponent cards)
 * @param trumpSuit - The trump suit
 * @param defenderHandSize - Number of cards in defender's hand
 * @param totalUnknownCards - Total unknown cards (deck + unknown opponent hands)
 * @param knownDefenderCards - Optional array of cards we KNOW the defender has (from pickups they haven't played)
 * @returns Probability between 0 and 1
 */
function calculateCoverProbabilityDP(
    attacks: Card[],
    excludedCards: Set<string>,
    trumpSuit: number,
    defenderHandSize: number,
    totalUnknownCards: number,
    knownDefenderCards: Card[] = []
): number {
    // Debug mode for tests with known cards
    const debugMode = false; // Set to true to enable debug output
    
    // Build buckets: group unknown cards by which attacks they can cover
    const bucketCounts: Record<string, number> = {};
    const bucketCards: Record<string, string[]> = {}; // For debugging
    
    SUITS.forEach(suit => {
        for (let value = 6 - 1; value <= 14 - 1; value++) {
            const defenseCard = { suit, value };
            const defenseKey = cardDisplay(defenseCard);
            
            if (!excludedCards.has(defenseKey)) {
                // Create bitmask: bit i is 1 if this card can cover attack i
                // Build string and reverse it (binary is right-to-left, but we build left-to-right)
                const bucketStr = attacks.map(attack => 
                    canCover(attack, defenseCard, trumpSuit) ? '1' : '0'
                ).reverse().join('');
                const bucket = '0b' + bucketStr;
                
                if (!bucketCounts[bucket]) {
                    bucketCounts[bucket] = 0;
                    bucketCards[bucket] = [];
                }
                bucketCounts[bucket]++;
                if (debugMode && knownDefenderCards.length > 0) {
                    bucketCards[bucket].push(defenseKey);
                }
            }
        }
    });
    
    // Sort buckets by size (smallest first for optimization)
    const sortedBuckets = Object.entries(bucketCounts)
        .sort((a, b) => a[1] - b[1]);
    
    // Calculate ACTUAL total unknown cards from buckets (not from parameter)
    let actualUnknownCards = 0;
    for (const [, count] of sortedBuckets) {
        actualUnknownCards += count;
    }
    
    // Helper function to decode achievability bitmask into subset list
    const decodeAchievability = (A: bigint, numAttacks: number): string => {
        const subsets: string[] = [];
        const totalSubsets = 1 << numAttacks;
        for (let subset = 0; subset < totalSubsets; subset++) {
            if ((A & (1n << BigInt(subset))) !== 0n) {
                // This subset is achievable, decode which attacks it represents
                const attackIndices: number[] = [];
                for (let i = 0; i < numAttacks; i++) {
                    if (subset & (1 << i)) {
                        attackIndices.push(i);
                    }
                }
                if (attackIndices.length === 0) {
                    subsets.push('{}');
                } else {
                    subsets.push(`{${attackIndices.join(',')}}`);
                }
            }
        }
        return subsets.join(', ');
    };
    
    if (debugMode && knownDefenderCards.length > 0) {
        console.log(`\n[DEBUG] Bucket analysis for ${attacks.length} attacks:`);
        console.log(`  Attack mappings:`);
        for (let i = 0; i < attacks.length; i++) {
            console.log(`    Attack ${i}: ${cardDisplay(attacks[i])}`);
        }
        console.log(`  Total buckets: ${sortedBuckets.length}`);
        console.log(`  Total cards in buckets: ${actualUnknownCards}`);
        console.log(`  Excluded cards:`, Array.from(excludedCards).join(', '));
        // Show buckets
        console.log(`  Buckets:`);
        for (const [bucket, count] of sortedBuckets) {
            const mask = Number(bucket);
            const coversList: string[] = [];
            for (let i = 0; i < attacks.length; i++) {
                if (mask & (1 << i)) {
                    coversList.push(String(i));
                }
            }
            const cards = bucketCards[bucket] || [];
            console.log(`    ${bucket} (covers attacks ${coversList.join(',') || 'none'}): ${count} cards - ${cards.join(', ')}`);
        }
        // Show which cards can cover each attack
        for (let i = 0; i < attacks.length; i++) {
            const attack = attacks[i];
            let canCoverCount = 0;
            const canCoverList: string[] = [];
            SUITS.forEach(suit => {
                for (let value = 6 - 1; value <= 14 - 1; value++) {
                    const defenseCard = { suit, value };
                    const defenseKey = cardDisplay(defenseCard);
                    if (!excludedCards.has(defenseKey) && canCover(attack, defenseCard, trumpSuit)) {
                        canCoverCount++;
                        if (canCoverList.length < 10) canCoverList.push(defenseKey);
                    }
                }
            });
            console.log(`  Attack ${i} (${cardDisplay(attack)}): ${canCoverCount} cards can cover: ${canCoverList.join(', ')}`);
        }
    }
    
    const subsetCount = 1 << attacks.length;
    const fullSet = subsetCount - 1;
    
    // Quick pre-check: OR all bucket masks together
    let combinedMask = 0;
    for (const bucket of sortedBuckets) {
        combinedMask |= Number(bucket[0]);
    }
    if (combinedMask !== fullSet) {
        // There exists an attack that cannot be covered by any card
        return 0;
    }
    
    // Separate useless cards bucket from useful ones
    let uselessBucket: [string, number] | null = null;
    const usefulBuckets = sortedBuckets.filter(bucket => {
        if (Number(bucket[0]) === 0) {
            uselessBucket = bucket;
            return false;
        }
        return true;
    });
    
    // Initialize DP state
    let dp: Record<number, Record<string, bigint>>;
    let remainingHandSize = defenderHandSize;
    
    if (knownDefenderCards.length > 0) {
        // Start with known cards: enumerate all subsets and their achievability
        dp = initializeDPWithKnownCards(knownDefenderCards, attacks, trumpSuit, subsetCount);
        remainingHandSize = defenderHandSize - knownDefenderCards.length;
        if (debugMode) {
            console.log('\n[DEBUG] After initializing with known cards:');
            console.log(`  remainingHandSize = ${remainingHandSize}`);
            for (const [r, states] of Object.entries(dp)) {
                console.log(`  dp[${r}]:`);
                for (const [AStr, ways] of Object.entries(states)) {
                    const achievableSets = decodeAchievability(BigInt(AStr), attacks.length);
                    console.log(`    Achievable: ${achievableSets} (${ways} ways)`);
                }
            }
        }
    } else {
        // Initial state: A0 = 1n (only subset 0, the empty set, is achievable)
        const A0 = 1n << 0n;
        dp = {
            [0]: { [A0.toString()]: 1n }
        };
    }
    
    // Calculate full target hand size
    const fullHandSize = knownDefenderCards.length + remainingHandSize;
    
    // Process all useful buckets for remaining hand slots
    for (const bucket of usefulBuckets) {
        const bucketMask = Number(bucket[0]);
        const bucketSize = bucket[1];
        dp = applyBucket(dp, bucketSize, bucketMask, fullHandSize, subsetCount, false);
    }
    
    if (debugMode) {
        console.log('\n[DEBUG] After processing useful buckets:');
        for (const [r, states] of Object.entries(dp)) {
            if (Object.keys(states).length > 0) {
                console.log(`  dp[${r}]: ${Object.keys(states).length} states`);
                for (const [AStr, ways] of Object.entries(states)) {
                    const achievableSets = decodeAchievability(BigInt(AStr), attacks.length);
                    console.log(`    Achievable: ${achievableSets} (${ways} ways)`);
                }
            }
        }
    }
    
    // Filter: keep only states that achieve fullSet
    const fullSetBit = 1n << BigInt(fullSet);
    if (debugMode) {
        console.log(`\n[DEBUG] Filtering for fullSet=${fullSet}, fullSetBit=${fullSetBit}`);
    }
    for (let r = 0; r <= fullHandSize; r++) {
        if (!dp[r]) continue;
        const newRow: Record<string, bigint> = {};
        for (const [AStr, ways] of Object.entries(dp[r])) {
            const A = BigInt(AStr);
            if ((A & fullSetBit) !== 0n) {
                newRow[AStr] = ways;
            }
        }
        if (Object.keys(newRow).length > 0) {
            dp[r] = newRow;
        } else {
            delete dp[r];
        }
    }
    
    if (debugMode) {
        console.log('\n[DEBUG] After filtering:');
        for (const [r, states] of Object.entries(dp)) {
            if (Object.keys(states).length > 0) {
                console.log(`  dp[${r}]: ${Object.keys(states).length} states`);
                for (const [AStr, ways] of Object.entries(states)) {
                    const achievableSets = decodeAchievability(BigInt(AStr), attacks.length);
                    console.log(`    Achievable: ${achievableSets} (${ways} ways)`);
                }
            }
        }
    }
    
    // Process useless cards bucket
    if (uselessBucket) {
        if (debugMode) {
            console.log(`\n[DEBUG] Applying useless bucket: size=${uselessBucket[1]}`);
        }
        dp = applyBucket(dp, uselessBucket[1], 0, fullHandSize, subsetCount, true);
        if (debugMode) {
            console.log('\n[DEBUG] After useless bucket:');
            for (const [r, states] of Object.entries(dp)) {
                if (Object.keys(states).length > 0) {
                    console.log(`  dp[${r}]: ${Object.keys(states).length} states`);
                    for (const [AStr, ways] of Object.entries(states)) {
                        const achievableSets = decodeAchievability(BigInt(AStr), attacks.length);
                        console.log(`    Achievable: ${achievableSets} (${ways} ways)`);
                    }
                }
            }
        }
    }
    
    // Calculate total possible hands using ACTUAL unknown cards from buckets
    // (not the parameter, which may represent something different in the calling context)
    const totalHands = BigInt(combination(actualUnknownCards, remainingHandSize));
    
    // Sum all states at full hand size that can achieve fullSet
    let success = 0n;
    const dpH = dp[fullHandSize];
    
    // Debug logging (can be removed later)
    if (debugMode) {
        console.log('\n[DEBUG] Final DP state:');
        console.log(`  fullHandSize = ${fullHandSize}`);
        console.log(`  remainingHandSize = ${remainingHandSize}`);
        console.log(`  actualUnknownCards = ${actualUnknownCards}`);
        console.log(`  totalUnknownCards (param) = ${totalUnknownCards}`);
        console.log(`  fullSet = ${fullSet.toString(2)} (binary) = {${Array.from({length: attacks.length}, (_, i) => i).join(',')}}`);
        console.log(`  fullSetBit = ${fullSetBit.toString()}`);
        if (dpH) {
            console.log(`  dp[${fullHandSize}]:`);
            for (const [AStr, ways] of Object.entries(dpH)) {
                const A = BigInt(AStr);
                const hasFull = (A & fullSetBit) !== 0n;
                const achievableSets = decodeAchievability(A, attacks.length);
                console.log(`    Achievable: ${achievableSets}`);
                console.log(`      ${ways} ways, hasFull=${hasFull}`);
            }
        } else {
            console.log(`  dp[${fullHandSize}] is undefined!`);
        }
        console.log(`  totalHands = C(${actualUnknownCards}, ${remainingHandSize}) = ${totalHands.toString()}`);
    }
    
    if (dpH) {
        for (const [AStr, ways] of Object.entries(dpH)) {
            const A = BigInt(AStr);
            const hasFull = (A & fullSetBit) !== 0n;
            if (hasFull) success += ways as bigint;
        }
    }
    
    if (totalHands === 0n) return 0;
    return Number(success) / Number(totalHands);
}

// ============================================================================
// ENHANCED DP: Track "good rank" cards for P(CoverAllowsAtk | Cover)
// ============================================================================

/**
 * Add one card to achievable subsets, tracking (A0, A1) state pair.
 * A0 = achievable with NO good-rank cards used as covers
 * A1 = achievable with AT LEAST ONE good-rank card used as covers
 * 
 * Transition rules:
 * - From A0 with g=0 card → stays in A0
 * - From A0 with g=1 card → flips to A1
 * - From A1 with any card → stays in A1
 */
function addOneCardAchievablePair(
    A0: bigint,
    A1: bigint,
    coverMask: number,
    isGoodRank: boolean,
    subsetCount: number
): { A0: bigint; A1: bigint } {
    let newA0 = A0;
    let newA1 = A1;
    
    // For each currently achievable subset in A0
    for (let S = 0; S < subsetCount; S++) {
        const bitS = 1n << BigInt(S);
        if ((A0 & bitS) !== 0n) {
            // Which attacks does this card cover that aren't already in S?
            const available = coverMask & (~S);
            for (const b of bitsOf(available)) {
                const S2 = S | b;
                const bitS2 = 1n << BigInt(S2);
                if (isGoodRank) {
                    // Using a good-rank card → flip to A1
                    newA1 |= bitS2;
                } else {
                    // Using a non-good-rank card → stay in A0
                    newA0 |= bitS2;
                }
            }
        }
    }
    
    // For each currently achievable subset in A1
    for (let S = 0; S < subsetCount; S++) {
        const bitS = 1n << BigInt(S);
        if ((A1 & bitS) !== 0n) {
            const available = coverMask & (~S);
            for (const b of bitsOf(available)) {
                const S2 = S | b;
                // Always stay in A1 (we already used a good-rank card)
                newA1 |= 1n << BigInt(S2);
            }
        }
    }
    
    return { A0: newA0, A1: newA1 };
}

// Cache for addOneCardAchievablePair
const addOneCardPairCache = new Map<string, { A0: bigint; A1: bigint }>();

function addOneCardAchievablePairCached(
    A0: bigint,
    A1: bigint,
    coverMask: number,
    isGoodRank: boolean,
    subsetCount: number
): { A0: bigint; A1: bigint } {
    const key = `${A0}|${A1}|${coverMask}|${isGoodRank}|${subsetCount}`;
    let result = addOneCardPairCache.get(key);
    if (result === undefined) {
        result = addOneCardAchievablePair(A0, A1, coverMask, isGoodRank, subsetCount);
        addOneCardPairCache.set(key, result);
    }
    return result;
}

/**
 * Apply a bucket of cards to the DP state for the enhanced (A0, A1) tracking.
 * Now buckets are keyed by (mask, isGoodRank).
 */
function applyBucketPair(
    dp: Record<number, Record<string, bigint>>,
    bucketSize: number,
    bucketMask: number,
    isGoodRank: boolean,
    maxHandSize: number,
    subsetCount: number,
    isUseless = false
): Record<number, Record<string, bigint>> {
    const nextDp: Record<number, Record<string, bigint>> = {};
    const applyBucketMemo = isUseless ? null : new Map<string, { A0: bigint; A1: bigint }>();
    
    for (let r = 0; r <= maxHandSize; r++) {
        const row = dp[r];
        if (!row) continue;
        
        for (const [stateStr, ways] of Object.entries(row)) {
            const waysAsBigInt = typeof ways === 'bigint' ? ways : BigInt(ways as string);
            
            // Parse state: "A0|A1"
            const [A0Str, A1Str] = stateStr.split('|');
            const A0 = BigInt(A0Str);
            const A1 = BigInt(A1Str);
            
            for (let t = 0; t + r <= maxHandSize; t++) {
                const combos = combination(bucketSize, t);
                if (combos == 0) continue;
                
                let newStateStr: string;
                if (isUseless) {
                    // Useless cards don't change achievability
                    newStateStr = stateStr;
                } else {
                    // Apply bucket mask t times
                    const memoKey = `${stateStr}|${t}`;
                    let result = applyBucketMemo!.get(memoKey);
                    if (result === undefined) {
                        result = { A0, A1 };
                        for (let i = 0; i < t; i++) {
                            result = addOneCardAchievablePairCached(
                                result.A0, result.A1, bucketMask, isGoodRank, subsetCount
                            );
                        }
                        applyBucketMemo!.set(memoKey, result);
                    }
                    newStateStr = `${result.A0}|${result.A1}`;
                }
                
                const r2 = r + t;
                if (!nextDp[r2]) nextDp[r2] = {};
                if (!nextDp[r2][newStateStr]) nextDp[r2][newStateStr] = 0n;
                nextDp[r2][newStateStr] += waysAsBigInt * BigInt(combos);
            }
        }
    }
    return nextDp;
}

/**
 * Initialize DP with known defender cards for the (A0, A1) tracking.
 */
function initializeDPWithKnownCardsPair(
    knownCards: Card[],
    attacks: Card[],
    trumpSuit: number,
    goodRanks: Set<number>,
    subsetCount: number
): Record<number, Record<string, bigint>> {
    const dp: Record<number, Record<string, bigint>> = {};
    
    // Start with (A0=1, A1=0): only empty set achievable, no good-rank used
    let A0 = 1n << 0n;
    let A1 = 0n;
    
    for (const card of knownCards) {
        // Which attacks can this card cover?
        let cardMask = 0;
        for (let i = 0; i < attacks.length; i++) {
            if (canCover(attacks[i], card, trumpSuit)) {
                cardMask |= (1 << i);
            }
        }
        
        // Is this a good-rank card?
        const isGoodRank = goodRanks.has(card.value);
        
        // Update (A0, A1)
        const result = addOneCardAchievablePair(A0, A1, cardMask, isGoodRank, subsetCount);
        A0 = result.A0;
        A1 = result.A1;
    }
    
    const r = knownCards.length;
    dp[r] = {};
    dp[r][`${A0}|${A1}`] = 1n;
    
    return dp;
}

export interface CoverWithGoodRankResult {
    pCover: number;                  // P(defender can cover all attacks)
    pPossibleGoodRank: number;       // P(∃ cover using matching rank | cover)
    pForcedGoodRank: number;         // P(every cover uses matching rank | cover)
    debug?: {
        cCover: bigint;              // Total hands that can cover
        cNoGood: bigint;             // Hands that can cover WITHOUT using matching rank
        cHasGood: bigint;            // Hands that can cover using ≥1 matching rank
        goodRanks: number[];         // The ranks in attacker's remaining hand
        totalHands: bigint;
    };
}

/**
 * Calculate cover probability AND the conditional probabilities for 
 * whether the cover cards will have ranks matching the attacker's remaining hand.
 * 
 * @param attacks - Attack cards
 * @param excludedCards - Cards NOT in the unknown pool
 * @param trumpSuit - Trump suit
 * @param defenderHandSize - Defender's hand size
 * @param attackerRemainingHand - Attacker's remaining cards (after leading attacks)
 * @param knownDefenderCards - Known defender cards
 * @param debugMode - Enable debug output
 */
function calculateCoverProbabilityDPWithGoodRanks(
    attacks: Card[],
    excludedCards: Set<string>,
    trumpSuit: number,
    defenderHandSize: number,
    attackerRemainingHand: Card[],
    knownDefenderCards: Card[] = [],
    debugMode = false
): CoverWithGoodRankResult {
    // Step 1: Mark "good ranks" from attacker's remaining hand
    const goodRanks = new Set<number>(attackerRemainingHand.map(c => c.value));
    
    // Step 2 & 3: Build buckets keyed by (mask, isGoodRank)
    // Key format: "0bXXXX|0" or "0bXXXX|1"
    const bucketCounts: Record<string, number> = {};
    const bucketCards: Record<string, string[]> = {}; // For debugging
    
    SUITS.forEach(suit => {
        for (let value = 6 - 1; value <= 14 - 1; value++) {
            const defenseCard = { suit, value };
            const defenseKey = cardDisplay(defenseCard);
            
            if (!excludedCards.has(defenseKey)) {
                // Create bitmask
                const bucketMaskStr = attacks.map(attack => 
                    canCover(attack, defenseCard, trumpSuit) ? '1' : '0'
                ).reverse().join('');
                const bucketMask = '0b' + bucketMaskStr;
                
                // Is this a good-rank card?
                const isGoodRank = goodRanks.has(value) ? 1 : 0;
                
                // Bucket key: "mask|flag"
                const bucketKey = `${bucketMask}|${isGoodRank}`;
                
                if (!bucketCounts[bucketKey]) {
                    bucketCounts[bucketKey] = 0;
                    bucketCards[bucketKey] = [];
                }
                bucketCounts[bucketKey]++;
                if (debugMode) {
                    bucketCards[bucketKey].push(defenseKey);
                }
            }
        }
    });
    
    // Sort buckets by size (smallest first)
    const sortedBuckets = Object.entries(bucketCounts)
        .sort((a, b) => a[1] - b[1]);
    
    // Calculate actual unknown cards
    let actualUnknownCards = 0;
    for (const [, count] of sortedBuckets) {
        actualUnknownCards += count;
    }
    
    // Helper to decode (A0, A1) state for debug output
    const decodeStatePair = (A0: bigint, A1: bigint, numAttacks: number): string => {
        const totalSubsets = 1 << numAttacks;
        const a0Sets: string[] = [];
        const a1Sets: string[] = [];
        
        for (let subset = 0; subset < totalSubsets; subset++) {
            const attackIndices: number[] = [];
            for (let i = 0; i < numAttacks; i++) {
                if (subset & (1 << i)) attackIndices.push(i);
            }
            const setStr = attackIndices.length === 0 ? '{}' : `{${attackIndices.join(',')}}`;
            
            if ((A0 & (1n << BigInt(subset))) !== 0n) a0Sets.push(setStr);
            if ((A1 & (1n << BigInt(subset))) !== 0n) a1Sets.push(setStr);
        }
        
        return `A0=[${a0Sets.join(',')}] A1=[${a1Sets.join(',')}]`;
    };
    
    if (debugMode) {
        console.log(`\n[DEBUG] CoverWithGoodRanks analysis for ${attacks.length} attacks:`);
        console.log(`  Attack mappings:`);
        for (let i = 0; i < attacks.length; i++) {
            console.log(`    Attack ${i}: ${cardDisplay(attacks[i])}`);
        }
        console.log(`  Good ranks (from attacker's remaining hand): [${Array.from(goodRanks).map(v => VALUE_NAMES[v]).join(', ')}]`);
        console.log(`  Attacker's remaining hand: ${attackerRemainingHand.map(cardDisplay).join(', ')}`);
        console.log(`  Total buckets: ${sortedBuckets.length}`);
        console.log(`  Total cards in buckets: ${actualUnknownCards}`);
        console.log(`  Buckets (mask|isGoodRank):`);
        for (const [bucketKey, count] of sortedBuckets) {
            const [maskStr, flagStr] = bucketKey.split('|');
            const mask = Number(maskStr);
            const isGood = flagStr === '1';
            const coversList: string[] = [];
            for (let i = 0; i < attacks.length; i++) {
                if (mask & (1 << i)) coversList.push(String(i));
            }
            const cards = bucketCards[bucketKey] || [];
            console.log(`    ${bucketKey} (covers ${coversList.join(',') || 'none'}, good=${isGood}): ${count} cards - ${cards.join(', ')}`);
        }
    }
    
    const subsetCount = 1 << attacks.length;
    const fullSet = subsetCount - 1;
    
    // Quick pre-check: OR all bucket masks together
    let combinedMask = 0;
    for (const [bucketKey] of sortedBuckets) {
        const maskStr = bucketKey.split('|')[0];
        combinedMask |= Number(maskStr);
    }
    if (combinedMask !== fullSet) {
        // There exists an attack that cannot be covered
        return {
            pCover: 0,
            pPossibleGoodRank: 0,
            pForcedGoodRank: 0,
            debug: debugMode ? {
                cCover: 0n,
                cNoGood: 0n,
                cHasGood: 0n,
                goodRanks: Array.from(goodRanks),
                totalHands: 0n
            } : undefined
        };
    }
    
    // Separate useless cards bucket from useful ones
    const uselessBuckets: [string, number][] = [];
    const usefulBuckets = sortedBuckets.filter(bucket => {
        const maskStr = bucket[0].split('|')[0];
        if (Number(maskStr) === 0) {
            uselessBuckets.push(bucket);
            return false;
        }
        return true;
    });
    
    // Initialize DP state with (A0, A1) pairs
    let dp: Record<number, Record<string, bigint>>;
    let remainingHandSize = defenderHandSize;
    
    if (knownDefenderCards.length > 0) {
        dp = initializeDPWithKnownCardsPair(knownDefenderCards, attacks, trumpSuit, goodRanks, subsetCount);
        remainingHandSize = defenderHandSize - knownDefenderCards.length;
        if (debugMode) {
            console.log('\n[DEBUG] After initializing with known cards:');
            console.log(`  remainingHandSize = ${remainingHandSize}`);
            for (const [r, states] of Object.entries(dp)) {
                console.log(`  dp[${r}]:`);
                for (const [stateStr, ways] of Object.entries(states)) {
                    const [A0Str, A1Str] = stateStr.split('|');
                    const decoded = decodeStatePair(BigInt(A0Str), BigInt(A1Str), attacks.length);
                    console.log(`    ${decoded} (${ways} ways)`);
                }
            }
        }
    } else {
        // Initial state: A0 = 1 (empty set achievable), A1 = 0
        const initialState = `${1n}|${0n}`;
        dp = { [0]: { [initialState]: 1n } };
    }
    
    const fullHandSize = knownDefenderCards.length + remainingHandSize;
    
    // Process all useful buckets
    for (const [bucketKey, bucketSize] of usefulBuckets) {
        const [maskStr, flagStr] = bucketKey.split('|');
        const bucketMask = Number(maskStr);
        const isGoodRank = flagStr === '1';
        dp = applyBucketPair(dp, bucketSize, bucketMask, isGoodRank, fullHandSize, subsetCount, false);
    }
    
    if (debugMode) {
        console.log('\n[DEBUG] After processing useful buckets:');
        for (const [r, states] of Object.entries(dp)) {
            if (Object.keys(states).length > 0) {
                console.log(`  dp[${r}]: ${Object.keys(states).length} states`);
                for (const [stateStr, ways] of Object.entries(states)) {
                    const [A0Str, A1Str] = stateStr.split('|');
                    const decoded = decodeStatePair(BigInt(A0Str), BigInt(A1Str), attacks.length);
                    console.log(`    ${decoded} (${ways} ways)`);
                }
            }
        }
    }
    
    // Process useless cards buckets (they don't affect A0/A1 achievability, just hand size)
    for (const [bucketKey, bucketSize] of uselessBuckets) {
        const [maskStr, flagStr] = bucketKey.split('|');
        const isGoodRank = flagStr === '1';
        dp = applyBucketPair(dp, bucketSize, 0, isGoodRank, fullHandSize, subsetCount, true);
    }
    
    if (debugMode && uselessBuckets.length > 0) {
        console.log('\n[DEBUG] After useless buckets:');
        for (const [r, states] of Object.entries(dp)) {
            if (Object.keys(states).length > 0) {
                console.log(`  dp[${r}]: ${Object.keys(states).length} states`);
            }
        }
    }
    
    // Calculate totals at full hand size
    const totalHands = BigInt(combination(actualUnknownCards, remainingHandSize));
    const fullSetBit = 1n << BigInt(fullSet);
    
    let cCover = 0n;   // Hands where cover exists (A0[full] OR A1[full])
    let cNoGood = 0n;  // Hands where ∃ cover using NO good-rank cards (A0[full])
    let cHasGood = 0n; // Hands where ∃ cover using ≥1 good-rank card (A1[full])
    
    const dpH = dp[fullHandSize];
    if (dpH) {
        for (const [stateStr, ways] of Object.entries(dpH)) {
            const [A0Str, A1Str] = stateStr.split('|');
            const A0 = BigInt(A0Str);
            const A1 = BigInt(A1Str);
            
            const hasA0Full = (A0 & fullSetBit) !== 0n;
            const hasA1Full = (A1 & fullSetBit) !== 0n;
            
            // A hand contributes to cCover if it can cover (via either path)
            if (hasA0Full || hasA1Full) {
                cCover += ways as bigint;
            }
            if (hasA0Full) {
                cNoGood += ways as bigint;
            }
            if (hasA1Full) {
                cHasGood += ways as bigint;
            }
        }
    }
    
    if (debugMode) {
        console.log('\n[DEBUG] Final counts:');
        console.log(`  totalHands = C(${actualUnknownCards}, ${remainingHandSize}) = ${totalHands}`);
        console.log(`  cCover (can cover) = ${cCover}`);
        console.log(`  cNoGood (∃ cover with no good-rank) = ${cNoGood}`);
        console.log(`  cHasGood (∃ cover with ≥1 good-rank) = ${cHasGood}`);
        console.log(`  Note: A hand can have BOTH a "good" and "noGood" cover`);
    }
    
    // Calculate probabilities
    const pCover = totalHands === 0n ? 0 : Number(cCover) / Number(totalHands);
    const pPossibleGoodRank = cCover === 0n ? 0 : Number(cHasGood) / Number(cCover);
    const pForcedGoodRank = cCover === 0n ? 0 : 1 - Number(cNoGood) / Number(cCover);
    
    if (debugMode) {
        console.log('\n[DEBUG] Probabilities:');
        console.log(`  P(Cover) = ${cCover}/${totalHands} = ${(pCover * 100).toFixed(2)}%`);
        console.log(`  P(∃ cover using matching rank | cover) = ${cHasGood}/${cCover} = ${(pPossibleGoodRank * 100).toFixed(2)}%`);
        console.log(`  P(every cover uses matching rank | cover) = 1 - ${cNoGood}/${cCover} = ${(pForcedGoodRank * 100).toFixed(2)}%`);
    }
    
    return {
        pCover,
        pPossibleGoodRank,
        pForcedGoodRank,
        debug: debugMode ? {
            cCover,
            cNoGood,
            cHasGood,
            goodRanks: Array.from(goodRanks),
            totalHands
        } : undefined
    };
}

// ============================================================================
// ATTACK STATS
// ============================================================================

function calculateAttackStatsWithDebug(
    game: Game,
    attackerId: string,
    move: LegalMove,
    tracker: CardTracker
): { stats: AttackStats; debug: AttackDebugInfo } {
    const cards = move.cards || [];
    const defenderId = game.players[game.defender].player_id;
    const defender = game.players[game.defender];
    const attackValue = cards[0]?.value; // All attack cards have same value
    
    // Get known cards for defender
    const defenderKnownCards = tracker.knownCardsByPlayer.get(defenderId) || new Set<string>();
    
    // Can definitely cover: defender has known cards that can cover ALL attacks
    const canDefinitelyCover = canDefinitelyCoverAttacks(game, cards, defenderKnownCards, tracker);
    
    // Definitely cannot cover: all covering cards are accounted for
    const definitelyCannotCover = definitelyCannotCoverAttacks(game, cards, tracker);
    
    // Pass back analysis - consider player positions
    const { canDefinitelyPassBack, definitelyCannotPassBack, seatsBetween } = analyzePassBackWithDebug(
        game, attackerId, attackValue, cards.length, tracker
    );
    
    // Debug info collection
    // unknownTotal = deck + unknown cards in opponent hands (not including known cards)
    const unknownTotal = game.deck.length + tracker.getUnknownCardCount();
    const defenderUnknownHand = Math.max(0, defender.hand.length - defenderKnownCards.size);
    
    // Count cover cards for first attack (representative)
    let coverCardsCount = 0;
    if (cards[0]) {
        // Durak uses cards 6-Ace, which are values 5-13 in internal representation
        for (let suit = 0; suit < 4; suit++) {
            for (let value = 5; value <= 13; value++) {
                if (canCover(cards[0], { suit, value }, game.power_suit)) {
                    if (!isCardFullyAccountedFor(`${suit}-${value}`, tracker)) {
                        coverCardsCount++;
                    }
                }
            }
        }
    }
    
    // Count value cards remaining for pass
    let valueCardsRemaining = 0;
    for (let suit = 0; suit < 4; suit++) {
        if (!isCardFullyAccountedFor(`${suit}-${attackValue}`, tracker)) {
            valueCardsRemaining++;
        }
    }
    
    // Probability calculations using DP with good ranks
    // Get attacker for hand analysis
    const attacker = game.players.find(p => p.player_id === attackerId);
    const playerUnknownHand = attacker ? Math.max(0, attacker.hand.length - (tracker.knownCardsByPlayer.get(attackerId)?.size || 0)) : 0;
    
    // Attacker's remaining hand (cards not used in this attack)
    const attackerRemainingHand = attacker ? attacker.hand.filter(c => 
        !cards.some(ac => ac.suit === c.suit && ac.value === c.value)
    ) : [];
    
    // Build excluded cards set
    const excludedCards = new Set<string>();
    for (let suit = 0; suit < 4; suit++) {
        for (let value = 5; value <= 13; value++) {
            if (isCardFullyAccountedFor(`${suit}-${value}`, tracker)) {
                excludedCards.add(cardDisplay({ suit, value }));
            }
        }
    }
    
    // Convert known defender cards to array
    const knownDefenderCardsArray: Card[] = [];
    for (const key of defenderKnownCards) {
        const [suit, value] = key.split('-').map(Number);
        knownDefenderCardsArray.push({ suit, value });
    }
    
    // Use DP algorithm with good ranks for both P(Cover) and P(CoverAllowsAtk)
    const dpResult = calculateCoverProbabilityDPWithGoodRanks(
        cards,                      // attacks
        excludedCards,              // excluded cards
        game.power_suit,           // trump suit
        defender.hand.length,       // defender hand size
        attackerRemainingHand,      // attacker's remaining cards (determines "good ranks")
        knownDefenderCardsArray,    // known defender cards
        false                       // debug mode off
    );
    
    const probCover = dpResult.pCover;
    // P(CoverAllowsAtk) = P(every cover MUST use matching rank | cover)
    // If they have no choice but to use a matching rank, we can continue attacking
    const probCoverAllowsAttack = dpResult.pForcedGoodRank;
    
    const probPass = calculatePassProbability(game, attackerId, attackValue, cards.length, tracker);
    
    const stats: AttackStats = {
        canDefinitelyCover,
        definitelyCannotCover,
        canDefinitelyPassBack,
        definitelyCannotPassBack,
        probCover,
        probPass,
        probCoverAllowsAttack
    };
    
    const debug: AttackDebugInfo = {
        unknownTotal,
        coverCardsCount,
        defenderUnknownHand,
        playersInPassChain: seatsBetween,
        attackValue,
        valueCardsRemaining,
        playerUnknownHand,
        coversMatchingMyHand: 0,  // Now calculated via DP
        totalPossibleCovers: 0    // Now calculated via DP
    };
    
    return { stats, debug };
}

function canDefinitelyCoverAttacks(
    game: Game,
    attackCards: Card[],
    defenderKnownCards: Set<string>,
    tracker: CardTracker
): boolean {
    // For each attack card, check if defender has a known card that can cover it
    const knownCoverCards: Card[] = [];
    for (const key of defenderKnownCards) {
        const [suit, value] = key.split('-').map(Number);
        knownCoverCards.push({ suit, value });
    }
    
    // Try to assign known cards to cover each attack
    const usedCards = new Set<string>();
    for (const attack of attackCards) {
        let foundCover = false;
        for (const cover of knownCoverCards) {
            const coverKey = `${cover.suit}-${cover.value}`;
            if (!usedCards.has(coverKey) && canCover(attack, cover, game.power_suit)) {
                usedCards.add(coverKey);
                foundCover = true;
                break;
            }
        }
        if (!foundCover) return false;
    }
    return true;
}

function definitelyCannotCoverAttacks(
    game: Game,
    attackCards: Card[],
    tracker: CardTracker
): boolean {
    // Check if ALL cards that could cover are accounted for (not with defender)
    const defenderId = game.players[game.defender].player_id;
    const defender = game.players[game.defender];
    
    for (const attack of attackCards) {
        let possibleCoverCount = 0;
        
        // Check all cards that could cover this attack
        for (let suit = 0; suit < 4; suit++) {
            for (let value = 5; value <= 13; value++) {
                const card = { suit, value };
                if (canCover(attack, card, game.power_suit)) {
                    // Is this card possibly with the defender?
                    const key = `${suit}-${value}`;
                    if (!isCardAccountedForElsewhere(key, defenderId, tracker)) {
                        possibleCoverCount++;
                    }
                }
            }
        }
        
        if (possibleCoverCount > 0) return false;
    }
    return true;
}

function analyzePassBackWithDebug(
    game: Game,
    attackerId: string,
    attackValue: number,
    numAttackCards: number,
    tracker: CardTracker
): { canDefinitelyPassBack: boolean; definitelyCannotPassBack: boolean; seatsBetween: number } {
    // Count how many players from defender to attacker (going clockwise)
    // This is the number of players who need the value for it to pass back
    const attackerIndex = game.players.findIndex(p => p.player_id === attackerId);
    const defenderIndex = game.defender;
    const defenderId = game.players[defenderIndex].player_id;
    
    // Count seats from defender to attacker (going clockwise, including defender, excluding attacker)
    let playersInPassChain = 0;
    let currentIndex = defenderIndex;
    while (currentIndex !== attackerIndex) {
        playersInPassChain++;
        currentIndex = get_next_player_index(game, currentIndex);
        if (playersInPassChain > game.players.length) break; // Safety
    }
    
    // Current table attacks + new attacks
    const totalAttacks = game.table_battles.length + numAttackCards;
    
    // Each player in the pass chain needs to be able to accept the attacks
    // If total attacks > players in chain, someone can't accept
    if (totalAttacks > playersInPassChain) {
        return { canDefinitelyPassBack: false, definitelyCannotPassBack: true, seatsBetween: playersInPassChain };
    }
    
    // Check if ALL players in the pass chain have the attack value
    let allHaveValue = true;
    currentIndex = defenderIndex;
    for (let i = 0; i < playersInPassChain; i++) {
        const playerId = game.players[currentIndex].player_id;
        const knownCards = tracker.knownCardsByPlayer.get(playerId) || new Set<string>();
        
        let hasValue = false;
        for (const key of knownCards) {
            const [, value] = key.split('-').map(Number);
            if (value === attackValue) {
                hasValue = true;
                break;
            }
        }
        if (!hasValue) {
            allHaveValue = false;
            break;
        }
        currentIndex = get_next_player_index(game, currentIndex);
    }
    
    // Check if all cards of this value are accounted for
    const allValueCardsAccountedFor = areAllValueCardsAccountedFor(game, attackerId, attackValue, tracker);
    
    return {
        canDefinitelyPassBack: allHaveValue,
        definitelyCannotPassBack: allValueCardsAccountedFor,
        seatsBetween: playersInPassChain
    };
}

function areAllValueCardsAccountedFor(
    game: Game,
    attackerId: string,
    value: number,
    tracker: CardTracker
): boolean {
    // Check all 4 suits for this value
    const DEBUG_PASSBACK = false; // Set to true to debug
    if (DEBUG_PASSBACK) {
        console.log(`[DEBUG areAllValueCardsAccountedFor] value=${value} (${VALUE_NAMES[value]})`);
    }
    for (let suit = 0; suit < 4; suit++) {
        const key = `${suit}-${value}`;
        const isAccounted = isCardFullyAccountedFor(key, tracker);
        if (DEBUG_PASSBACK) {
            console.log(`  suit=${suit}, key=${key}, isAccountedFor=${isAccounted}`);
        }
        // If this card is not accounted for, it could be with someone who could pass back
        if (!isAccounted) {
            if (DEBUG_PASSBACK) console.log(`  -> returning FALSE (not all accounted)`);
            return false;
        }
    }
    if (DEBUG_PASSBACK) console.log(`  -> returning TRUE (all accounted)`);
    return true;
}

function isCardAccountedForElsewhere(
    cardKey: string,
    excludePlayerId: string,
    tracker: CardTracker
): boolean {
    // Check if card is in my hand
    const myPlayer = tracker['game'].players.find(p => p.player_id === tracker['myPlayerId']);
    if (myPlayer) {
        for (const card of myPlayer.hand) {
            if (`${card.suit}-${card.value}` === cardKey) return true;
        }
    }
    
    // Check if card is discarded
    if (tracker['discardedCards'].has(cardKey)) return true;
    
    // Check if card is the flipped card (visible to everyone)
    if (tracker['flippedCard'] === cardKey) return true;
    
    // Check if card is known to be with another player (not excludePlayerId)
    for (const [playerId, knownCards] of tracker.knownCardsByPlayer.entries()) {
        if (playerId !== excludePlayerId && knownCards.has(cardKey)) {
            return true;
        }
    }
    
    return false;
}

function isCardFullyAccountedFor(cardKey: string, tracker: CardTracker): boolean {
    // Check my hand
    const myPlayer = tracker['game'].players.find(p => p.player_id === tracker['myPlayerId']);
    if (myPlayer) {
        for (const card of myPlayer.hand) {
            if (`${card.suit}-${card.value}` === cardKey) return true;
        }
    }
    
    // Check discarded
    if (tracker['discardedCards'].has(cardKey)) return true;
    
    // Check flipped card (visible to everyone)
    if (tracker['flippedCard'] === cardKey) return true;
    
    // Check all known cards
    for (const knownCards of tracker.knownCardsByPlayer.values()) {
        if (knownCards.has(cardKey)) return true;
    }
    
    return false;
}

function calculateCoverProbability(
    game: Game,
    attackCards: Card[],
    tracker: CardTracker
): number {
    if (attackCards.length === 0) return 1.0;
    
    const defender = game.players[game.defender];
    const defenderHandSize = defender.hand.length;
    const defenderKnownCards = tracker.knownCardsByPlayer.get(defender.player_id) || new Set<string>();
    
    // Build set of excluded cards (cards NOT in the unknown pool: our hand, discard, flipped, known opponent cards)
    const excludedCards = new Set<string>();
    
    // Add defender's known cards (they're excluded from unknown pool since we know they're in defender's hand)
    for (const key of defenderKnownCards) {
        const [suit, value] = key.split('-').map(Number);
        excludedCards.add(cardDisplay({ suit, value }));
    }
    
    // Add flipped card
    if (game.flipped) {
        excludedCards.add(cardDisplay(game.flipped));
    }
    
    // Add all cards from the tracker that are accounted for
    for (let suit = 0; suit < 4; suit++) {
        for (let value = 5; value <= 13; value++) {
            if (isCardFullyAccountedFor(`${suit}-${value}`, tracker)) {
                excludedCards.add(cardDisplay({ suit, value }));
            }
        }
    }
    
    // Calculate unknown total
    const unknownTotal = game.deck.length + tracker.getUnknownCardCount();
    const unknownInDefenderHand = Math.max(0, defenderHandSize - defenderKnownCards.size);
    
    if (unknownTotal <= 0 || unknownInDefenderHand <= 0) {
        // No unknown cards, so can only cover if known cards suffice
        // This is handled by canDefinitelyCover check
        return 0;
    }
    
    // Convert known defender cards from Set<string> to Card[]
    const knownDefenderCardsArray: Card[] = [];
    for (const key of defenderKnownCards) {
        const [suit, value] = key.split('-').map(Number);
        knownDefenderCardsArray.push({ suit, value });
    }
    
    // Use DP-based calculation for accurate probability
    return calculateCoverProbabilityDP(
        attackCards,
        excludedCards,
        game.power_suit,
        defenderHandSize, // Pass full hand size
        unknownTotal,
        knownDefenderCardsArray // Pass known cards
    );
}

function calculatePassProbability(
    game: Game,
    attackerId: string,
    attackValue: number,
    numCards: number,
    tracker: CardTracker
): number {
    // Use the new inclusion-exclusion formula from pass_prob.ts
    // This correctly handles the constraint that each passer needs at least 1 card of the value
    
    const defenderIndex = game.defender;
    const attackerIndex = game.players.findIndex(p => p.player_id === attackerId);
    const myPlayerId = attackerId; // For pass-back calculation, attacker is "us"
    
    // Build list of passer IDs (defender → ... → attacker, excluding attacker)
    const passerIds: string[] = [];
    let idx = defenderIndex;
    while (idx !== attackerIndex) {
        passerIds.push(game.players[idx].player_id);
        idx = get_next_player_index(game, idx);
        if (passerIds.length > game.players.length) break;
    }
    
    const distance = passerIds.length; // d
    const attackSize = game.table_battles.length + numCards; // a
    
    // Quick check: a + d > 4 means impossible
    if (isPassDefinitelyImpossible(attackSize, distance)) {
        return 0;
    }
    
    // Use preCheckPassWithTracker to get info about known cards
    const preCheck = preCheckPassWithTracker(
        game, tracker, attackValue, attackSize, passerIds, myPlayerId
    );
    
    if (preCheck.isDefinitelyImpossible) {
        return 0;
    }
    
    if (preCheck.isDefinitelyPossible) {
        return 1;
    }
    
    // Calculate probability using inclusion-exclusion formula
    // Get passer hand sizes
    const passerHandSizes = passerIds.map(passerId => {
        const player = game.players.find(p => p.player_id === passerId);
        return player?.hand.length ?? 6;
    });
    
    // Calculate total unknown cards (N)
    // N = deck + opponent hands - known opponent cards
    let totalUnknown = game.deck.length;
    for (const player of game.players) {
        if (player.player_id !== myPlayerId) {
            totalUnknown += player.hand.length;
        }
    }
    for (const [playerId, knownCards] of tracker.knownCardsByPlayer) {
        if (playerId !== myPlayerId) {
            totalUnknown -= knownCards.size;
        }
    }
    
    // R = remaining copies available for passing
    // If some copies are in my hand, flipped, discard, they're not available
    const effectiveR = preCheck.copiesUnknown + preCheck.copiesInPasserHands;
    
    const DEBUG_PASS_PROB = false; // Temporarily enable debug
    if (DEBUG_PASS_PROB) {
        console.log(`[DEBUG calculatePassProbability] value=${attackValue} (${VALUE_NAMES[attackValue]})`);
        console.log(`  distance=${distance}, attackSize=${attackSize}`);
        console.log(`  preCheck: copiesUnknown=${preCheck.copiesUnknown}, copiesInPasserHands=${preCheck.copiesInPasserHands}, copiesAccountedElsewhere=${preCheck.copiesAccountedElsewhere}`);
        console.log(`  effectiveR=${effectiveR}, totalUnknown=${totalUnknown}`);
        console.log(`  passerHandSizes=[${passerHandSizes.join(', ')}]`);
    }
    
    if (effectiveR <= 0) {
        if (DEBUG_PASS_PROB) console.log(`  -> returning 0 (effectiveR <= 0)`);
        return 0;
    }
    
    // Use the inclusion-exclusion formula with effectiveR override
    // This handles cases where some copies of the rank are in our hand but not used in the attack
    const prob = calcPassProbFormula(distance, attackSize, totalUnknown, passerHandSizes, effectiveR);
    if (DEBUG_PASS_PROB) console.log(`  -> prob=${prob}`);
    return prob;
}

/**
 * Calculate probability that a player has at least one card of a given value
 * (Used for attack continuation probability, not pass-back)
 */
function calculateProbHasValue(
    player: { hand: Card[] },
    value: number,
    knownCards: Set<string>,
    tracker: CardTracker
): number {
    const handSize = player.hand.length;
    const knownSize = knownCards.size;
    const unknownInHand = Math.max(0, handSize - knownSize);
    
    if (unknownInHand === 0) return 0;
    
    // Count unknown cards of this value
    let unknownValueCards = 0;
    for (let suit = 0; suit < 4; suit++) {
        const key = `${suit}-${value}`;
        if (!isCardFullyAccountedFor(key, tracker)) {
            unknownValueCards++;
        }
    }
    
    if (unknownValueCards === 0) return 0;
    
    // unknownTotal = deck + unknown cards in all opponent hands
    const game = tracker['game'] as Game;
    const unknownTotal = game.deck.length + tracker.getUnknownCardCount();
    if (unknownTotal <= 0) return 0;
    
    // P(at least one) = 1 - P(none)
    const probNone = hypergeometricProbZero(unknownTotal, unknownValueCards, unknownInHand);
    return 1 - probNone;
}

function calculateProbCoverAllowsAttackWithDebug(
    game: Game,
    attackerId: string,
    attackCards: Card[],
    tracker: CardTracker
): { prob: number; matchCount: number; totalCount: number } {
    // Get attacker's hand
    const attacker = game.players.find(p => p.player_id === attackerId);
    if (!attacker) return { prob: 0, matchCount: 0, totalCount: 0 };
    
    // Values currently on table + attack values
    const tableValues = new Set<number>();
    for (const battle of game.table_battles) {
        tableValues.add(battle.attack.value);
        if (battle.defense) tableValues.add(battle.defense.value);
    }
    for (const card of attackCards) {
        tableValues.add(card.value);
    }
    
    // For each possible cover, check if its value is in attacker's hand
    let totalProb = 0;
    let scenarios = 0;
    let totalMatchCount = 0;
    let totalPossibleCount = 0;
    
    // Simplified: average probability that a random cover value matches attacker's hand
    const attackerValues = new Set(attacker.hand.map(c => c.value));
    
    // For each attack, find UNKNOWN possible covers
    for (const attack of attackCards) {
        const possibleCovers: Card[] = [];
        for (let suit = 0; suit < 4; suit++) {
            for (let value = 5; value <= 13; value++) {
                const cardKey = `${suit}-${value}`;
                if (canCover(attack, { suit, value }, game.power_suit) && !isCardFullyAccountedFor(cardKey, tracker)) {
                    possibleCovers.push({ suit, value });
                }
            }
        }
        
        // Count covers that would allow additional attack
        let allowsAttackCount = 0;
        for (const cover of possibleCovers) {
            if (attackerValues.has(cover.value) && !tableValues.has(cover.value)) {
                allowsAttackCount++;
            }
        }
        
        totalMatchCount += allowsAttackCount;
        totalPossibleCount += possibleCovers.length;
        
        if (possibleCovers.length > 0) {
            totalProb += allowsAttackCount / possibleCovers.length;
            scenarios++;
        }
    }
    
    return { 
        prob: scenarios > 0 ? totalProb / scenarios : 0,
        matchCount: totalMatchCount,
        totalCount: totalPossibleCount
    };
}

// ============================================================================
// COVER STATS
// ============================================================================

function calculateCoverStatsWithDebug(
    game: Game,
    playerId: string,
    move: LegalMove,
    tracker: CardTracker
): { stats: CoverStats; debug: CoverDebugInfo } {
    const coverCards = move.cards || [];
    
    // Values that will be on table after this cover
    const tableValues = new Set<number>();
    for (const battle of game.table_battles) {
        tableValues.add(battle.attack.value);
        if (battle.defense) tableValues.add(battle.defense.value);
    }
    for (const card of coverCards) {
        tableValues.add(card.value);
    }
    
    // Probability that cover allows additional attack
    const { prob: probAllowsAdditionalAttack, debugInfo: attackDebug } = calculateProbOpponentHasTableValueWithDebug(game, playerId, coverCards, tableValues, tracker);
    
    // Probability that allows uncoverable attack
    const { prob: probAllowsUncoverableAttack, canCoverValues } = calculateProbUncoverableAttackWithDebug(game, playerId, coverCards, tableValues, tracker);
    
    // Probability of drawing better card
    const { prob: probDrawBetterCard, avgUnknownValue, cardValue } = calculateProbDrawBetterWithDebug(game, coverCards, tracker);
    
    const stats: CoverStats = {
        probAllowsAdditionalAttack,
        probAllowsUncoverableAttack,
        probDrawBetterCard
    };
    
    const debug: CoverDebugInfo = {
        unknownTotal: game.deck.length + tracker.getUnknownCardCount(),
        coverValue: coverCards[0]?.value || 0,
        valueCardsRemaining: attackDebug.valueCardsRemaining,
        attackerUnknownHand: attackDebug.attackerUnknownHand,
        canCoverValues,
        avgUnknownValue,
        cardValue
    };
    
    return { stats, debug };
}

function calculateProbOpponentHasTableValueWithDebug(
    game: Game,
    defenderId: string,
    coverCards: Card[],
    tableValues: Set<number>,
    tracker: CardTracker
): { prob: number; debugInfo: { valueCardsRemaining: number; attackerUnknownHand: number } } {
    // Check each attacker for probability they have a matching value
    let maxProb = 0;
    let maxAttackerUnknownHand = 0;
    let valueCardsRemaining = 0;
    
    for (let i = 0; i < game.players.length; i++) {
        if (i === game.defender) continue;
        const player = game.players[i];
        if (player.status !== PLAYER_STATUS.IN) continue;
        
        const knownCards = tracker.knownCardsByPlayer.get(player.player_id) || new Set<string>();
        const unknownHand = Math.max(0, player.hand.length - knownCards.size);
        
        // Check if they definitely have a matching value
        for (const key of knownCards) {
            const [, value] = key.split('-').map(Number);
            // Check if this is a NEW value being added by our cover
            for (const coverCard of coverCards) {
                if (coverCard.value === value) {
                    return { prob: 1.0, debugInfo: { valueCardsRemaining: 4, attackerUnknownHand: unknownHand } };
                }
            }
        }
        
        // Calculate probability for new values being added
        for (const coverCard of coverCards) {
            const prob = calculateProbHasValue(player, coverCard.value, knownCards, tracker);
            if (prob > maxProb) {
                maxProb = prob;
                maxAttackerUnknownHand = unknownHand;
                // Count value cards remaining for this value
                let count = 0;
                for (let suit = 0; suit < 4; suit++) {
                    if (!isCardFullyAccountedFor(`${suit}-${coverCard.value}`, tracker)) {
                        count++;
                    }
                }
                valueCardsRemaining = count;
            }
        }
    }
    
    return { prob: maxProb, debugInfo: { valueCardsRemaining, attackerUnknownHand: maxAttackerUnknownHand } };
}

function calculateProbUncoverableAttackWithDebug(
    game: Game,
    defenderId: string,
    coverCards: Card[],
    tableValues: Set<number>,
    tracker: CardTracker
): { prob: number; canCoverValues: string } {
    const defender = game.players.find(p => p.player_id === defenderId);
    if (!defender) return { prob: 0, canCoverValues: '(none)' };
    
    // Cards remaining in defender's hand after this cover
    const remainingHand = defender.hand.filter(c => 
        !coverCards.some(cc => cc.suit === c.suit && cc.value === c.value)
    );
    
    // Track which values remaining hand can cover
    const coverableValues = new Set<number>();
    for (const card of remainingHand) {
        // This card can cover attacks of lower values (same suit or trump over non-trump)
        coverableValues.add(card.value); // Can always cover same suit lower values
    }
    
    // For each new value being added, check if defender can cover attacks of that value
    let probUncoverable = 0;
    
    for (const coverCard of coverCards) {
        const newValue = coverCard.value;
        
        // Find the weakest possible attack with this value
        // (lowest suit that's not trump)
        let worstAttack: Card | null = null;
        for (let suit = 0; suit < 4; suit++) {
            if (suit !== game.power_suit) {
                worstAttack = { suit, value: newValue };
                break;
            }
        }
        if (!worstAttack) worstAttack = { suit: game.power_suit, value: newValue };
        
        // Can defender cover this?
        let canCoverIt = false;
        for (const card of remainingHand) {
            if (canCover(worstAttack, card, game.power_suit)) {
                canCoverIt = true;
                break;
            }
        }
        
        if (!canCoverIt) {
            // Defender definitely can't cover attacks of this value
            // Probability attacker has this value
            for (let i = 0; i < game.players.length; i++) {
                if (i === game.defender) continue;
                const player = game.players[i];
                if (player.status !== PLAYER_STATUS.IN) continue;
                
                const knownCards = tracker.knownCardsByPlayer.get(player.player_id) || new Set<string>();
                const prob = calculateProbHasValue(player, newValue, knownCards, tracker);
                probUncoverable = Math.max(probUncoverable, prob);
            }
        }
    }
    
    const canCoverValues = remainingHand.length > 0 
        ? [...new Set(remainingHand.map(c => c.value))].sort((a,b) => a-b).join(',')
        : '(empty hand)';
    
    return { prob: probUncoverable, canCoverValues };
}

function calculateProbDrawBetterWithDebug(
    game: Game,
    coverCards: Card[],
    tracker: CardTracker
): { prob: number; avgUnknownValue: number; cardValue: number } {
    if (game.deck.length === 0 && !game.flipped) {
        return { prob: 0, avgUnknownValue: 0, cardValue: coverCards[0]?.value || 0 };
    }
    
    const avgUnknownValue = tracker.getAverageUnknownCardValue();
    
    // For each cover card, probability of drawing something better
    let totalProb = 0;
    let firstCardValue = 0;
    for (const card of coverCards) {
        const cardValue = getCardValue(card, game.power_suit);
        if (!firstCardValue) firstCardValue = cardValue;
        // Rough approximation: if avg unknown > our card value, good chance of drawing better
        // More sophisticated: count cards better than ours
        const probBetter = avgUnknownValue > card.value ? 
            Math.min(0.9, (avgUnknownValue - card.value) / 10) : 
            Math.max(0.1, 1 - (card.value - avgUnknownValue) / 10);
        totalProb += probBetter;
    }
    
    return { 
        prob: coverCards.length > 0 ? totalProb / coverCards.length : 0,
        avgUnknownValue,
        cardValue: firstCardValue
    };
}

// ============================================================================
// PASS STATS (similar to attack)
// ============================================================================

function calculatePassStatsWithDebug(
    game: Game,
    passerId: string,
    move: LegalMove,
    tracker: CardTracker
): { stats: PassStats; debug: PassDebugInfo } {
    const cards = move.cards || [];
    const passValue = cards[0]?.value;
    
    // After pass, the next player becomes defender
    const currentDefenderIndex = game.defender;
    const newDefenderIndex = get_next_player_index(game, currentDefenderIndex);
    const newDefenderId = game.players[newDefenderIndex].player_id;
    const newDefender = game.players[newDefenderIndex];
    
    const defenderKnownCards = tracker.knownCardsByPlayer.get(newDefenderId) || new Set<string>();
    
    // Total attacks after pass
    const totalAttacks = game.table_battles.length + cards.length;
    
    // Create virtual attack cards (all with same value)
    const virtualAttacks: Card[] = [];
    for (const battle of game.table_battles) {
        virtualAttacks.push(battle.attack);
    }
    for (const card of cards) {
        virtualAttacks.push(card);
    }
    
    const canDefinitelyCover = canDefinitelyCoverAttacks(game, virtualAttacks, defenderKnownCards, tracker);
    const definitelyCannotCover = definitelyCannotCoverAttacksForPlayer(game, virtualAttacks, newDefenderId, tracker);
    
    // Pass back analysis for the new defender
    const { canDefinitelyPassBack, definitelyCannotPassBack, seatsBetween } = analyzePassBackFromNewDefenderWithDebug(
        game, passerId, newDefenderIndex, passValue, totalAttacks, tracker
    );
    
    // Get passer for hand analysis (passer is the one who can continue attacking after pass)
    const passer = game.players.find(p => p.player_id === passerId);
    
    // Passer's remaining hand (cards not used in this pass)
    const passerRemainingHand = passer ? passer.hand.filter(c => 
        !cards.some(pc => pc.suit === c.suit && pc.value === c.value)
    ) : [];
    
    // Build excluded cards set
    const excludedCards = new Set<string>();
    for (let suit = 0; suit < 4; suit++) {
        for (let value = 5; value <= 13; value++) {
            if (isCardFullyAccountedFor(`${suit}-${value}`, tracker)) {
                excludedCards.add(cardDisplay({ suit, value }));
            }
        }
    }
    
    // Convert known defender cards to array
    const knownDefenderCardsArray: Card[] = [];
    for (const key of defenderKnownCards) {
        const [suit, value] = key.split('-').map(Number);
        knownDefenderCardsArray.push({ suit, value });
    }
    
    // Use DP algorithm with good ranks
    const dpResult = calculateCoverProbabilityDPWithGoodRanks(
        virtualAttacks,             // attacks
        excludedCards,              // excluded cards
        game.power_suit,           // trump suit
        newDefender.hand.length,    // new defender hand size
        passerRemainingHand,        // passer's remaining cards (determines "good ranks")
        knownDefenderCardsArray,    // known defender cards
        false                       // debug mode off
    );
    
    const probCover = dpResult.pCover;
    // P(CoverAllowsAtk) = P(every cover MUST use matching rank | cover)
    // If they have no choice but to use a matching rank, we can continue attacking
    const probCoverAllowsAttack = dpResult.pForcedGoodRank;
    
    const probPass = calculatePassProbabilityFromNewDefender(game, passerId, newDefenderIndex, passValue, totalAttacks, tracker);
    
    // Debug info
    const unknownTotal = game.deck.length + tracker.getUnknownCardCount();
    const newDefenderUnknownHand = Math.max(0, newDefender.hand.length - defenderKnownCards.size);
    
    // Count cover cards
    let coverCardsCount = 0;
    if (virtualAttacks[0]) {
        for (let suit = 0; suit < 4; suit++) {
            for (let value = 5; value <= 13; value++) {
                if (canCover(virtualAttacks[0], { suit, value }, game.power_suit)) {
                    if (!isCardFullyAccountedFor(`${suit}-${value}`, tracker)) {
                        coverCardsCount++;
                    }
                }
            }
        }
    }
    
    const stats: PassStats = {
        canDefinitelyCover,
        definitelyCannotCover,
        canDefinitelyPassBack,
        definitelyCannotPassBack,
        probCover,
        probPass,
        probCoverAllowsAttack
    };
    
    const debug: PassDebugInfo = {
        unknownTotal,
        coverCardsCount,
        newDefenderUnknownHand,
        playersInPassChain: seatsBetween,
        seatsBetween,
        totalAttacks,
        coversMatchingMyHand: 0,  // Now calculated via DP
        totalPossibleCovers: 0    // Now calculated via DP
    };
    
    return { stats, debug };
}

function definitelyCannotCoverAttacksForPlayer(
    game: Game,
    attackCards: Card[],
    defenderId: string,
    tracker: CardTracker
): boolean {
    for (const attack of attackCards) {
        let possibleCoverCount = 0;
        
        for (let suit = 0; suit < 4; suit++) {
            for (let value = 5; value <= 13; value++) {
                const card = { suit, value };
                if (canCover(attack, card, game.power_suit)) {
                    const key = `${suit}-${value}`;
                    if (!isCardAccountedForElsewhere(key, defenderId, tracker)) {
                        possibleCoverCount++;
                    }
                }
            }
        }
        
        if (possibleCoverCount > 0) return false;
    }
    return true;
}

function analyzePassBackFromNewDefenderWithDebug(
    game: Game,
    originalPasserId: string,
    newDefenderIndex: number,
    passValue: number,
    totalAttacks: number,
    tracker: CardTracker
): { canDefinitelyPassBack: boolean; definitelyCannotPassBack: boolean; seatsBetween: number } {
    const passerIndex = game.players.findIndex(p => p.player_id === originalPasserId);
    
    // Count players from new defender back to original passer (clockwise)
    let playersInPassChain = 0;
    let currentIndex = newDefenderIndex;
    while (currentIndex !== passerIndex) {
        playersInPassChain++;
        currentIndex = get_next_player_index(game, currentIndex);
        if (playersInPassChain > game.players.length) break;
    }
    
    if (totalAttacks > playersInPassChain) {
        return { canDefinitelyPassBack: false, definitelyCannotPassBack: true, seatsBetween: playersInPassChain };
    }
    
    // Check if ALL players in chain have the value
    let allHaveValue = true;
    currentIndex = newDefenderIndex;
    for (let i = 0; i < playersInPassChain; i++) {
        const playerId = game.players[currentIndex].player_id;
        const knownCards = tracker.knownCardsByPlayer.get(playerId) || new Set<string>();
        
        let hasValue = false;
        for (const key of knownCards) {
            const [, value] = key.split('-').map(Number);
            if (value === passValue) {
                hasValue = true;
                break;
            }
        }
        if (!hasValue) {
            allHaveValue = false;
            break;
        }
        currentIndex = get_next_player_index(game, currentIndex);
    }
    
    const allAccountedFor = areAllValueCardsAccountedFor(game, originalPasserId, passValue, tracker);
    
    return {
        canDefinitelyPassBack: allHaveValue,
        definitelyCannotPassBack: allAccountedFor,
        seatsBetween: playersInPassChain
    };
}

function calculateCoverProbabilityForPlayer(
    game: Game,
    attackCards: Card[],
    defenderId: string,
    tracker: CardTracker
): number {
    if (attackCards.length === 0) return 1.0;
    
    const defender = game.players.find(p => p.player_id === defenderId);
    if (!defender) return 0;
    
    const defenderHandSize = defender.hand.length;
    const defenderKnownCards = tracker.knownCardsByPlayer.get(defenderId) || new Set<string>();
    
    // Build set of excluded cards (cards NOT in the unknown pool: our hand, discard, flipped, known opponent cards)
    const excludedCards = new Set<string>();
    
    // Add defender's known cards (they're excluded from unknown pool since we know they're in defender's hand)
    for (const key of defenderKnownCards) {
        const [suit, value] = key.split('-').map(Number);
        excludedCards.add(cardDisplay({ suit, value }));
    }
    
    // Add flipped card
    if (game.flipped) {
        excludedCards.add(cardDisplay(game.flipped));
    }
    
    // Add all cards from the tracker that are accounted for
    for (let suit = 0; suit < 4; suit++) {
        for (let value = 5; value <= 13; value++) {
            if (isCardFullyAccountedFor(`${suit}-${value}`, tracker)) {
                excludedCards.add(cardDisplay({ suit, value }));
            }
        }
    }
    
    // Calculate unknown total
    const unknownTotal = game.deck.length + tracker.getUnknownCardCount();
    const unknownInDefenderHand = Math.max(0, defenderHandSize - defenderKnownCards.size);
    
    if (unknownTotal <= 0 || unknownInDefenderHand <= 0) {
        return 0;
    }
    
    // Convert known defender cards from Set<string> to Card[]
    const knownDefenderCardsArray: Card[] = [];
    for (const key of defenderKnownCards) {
        const [suit, value] = key.split('-').map(Number);
        knownDefenderCardsArray.push({ suit, value });
    }
    
    // Use DP-based calculation
    return calculateCoverProbabilityDP(
        attackCards,
        excludedCards,
        game.power_suit,
        defenderHandSize, // Pass full hand size
        unknownTotal,
        knownDefenderCardsArray // Pass known cards
    );
}

function calculatePassProbabilityFromNewDefender(
    game: Game,
    originalPasserId: string,
    newDefenderIndex: number,
    passValue: number,
    totalAttacks: number,
    tracker: CardTracker
): number {
    // Use the new inclusion-exclusion formula from pass_prob.ts
    const passerIndex = game.players.findIndex(p => p.player_id === originalPasserId);
    
    // Build list of passer IDs (new defender → ... → original passer, excluding original passer)
    const passerIds: string[] = [];
    let idx = newDefenderIndex;
    while (idx !== passerIndex) {
        passerIds.push(game.players[idx].player_id);
        idx = get_next_player_index(game, idx);
        if (passerIds.length > game.players.length) break;
    }
    
    const distance = passerIds.length;
    const attackSize = totalAttacks;
    
    // Quick check: a + d > 4 means impossible
    if (isPassDefinitelyImpossible(attackSize, distance)) {
        return 0;
    }
    
    // Use preCheckPassWithTracker
    const preCheck = preCheckPassWithTracker(
        game, tracker, passValue, attackSize, passerIds, originalPasserId
    );
    
    if (preCheck.isDefinitelyImpossible) {
        return 0;
    }
    
    if (preCheck.isDefinitelyPossible) {
        return 1;
    }
    
    // Get passer hand sizes
    const passerHandSizes = passerIds.map(passerId => {
        const player = game.players.find(p => p.player_id === passerId);
        return player?.hand.length ?? 6;
    });
    
    // Calculate total unknown cards
    let totalUnknown = game.deck.length;
    for (const player of game.players) {
        if (player.player_id !== originalPasserId) {
            totalUnknown += player.hand.length;
        }
    }
    for (const [playerId, knownCards] of tracker.knownCardsByPlayer) {
        if (playerId !== originalPasserId) {
            totalUnknown -= knownCards.size;
        }
    }
    
    const effectiveR = preCheck.copiesUnknown + preCheck.copiesInPasserHands;
    
    if (effectiveR <= 0) {
        return 0;
    }
    
    // Use the inclusion-exclusion formula with effectiveR override
    // This handles cases where some copies of the rank are in our hand but not used in the attack
    return calcPassProbFormula(distance, attackSize, totalUnknown, passerHandSizes, effectiveR);
}

function calculateProbCoverAllowsAttackFromPasserWithDebug(
    game: Game,
    passerId: string,
    attackCards: Card[],
    tracker: CardTracker
): { prob: number; matchCount: number; totalCount: number } {
    const passer = game.players.find(p => p.player_id === passerId);
    if (!passer) return { prob: 0, matchCount: 0, totalCount: 0 };
    
    const tableValues = new Set<number>();
    for (const battle of game.table_battles) {
        tableValues.add(battle.attack.value);
        if (battle.defense) tableValues.add(battle.defense.value);
    }
    for (const card of attackCards) {
        tableValues.add(card.value);
    }
    
    const passerValues = new Set(passer.hand.map(c => c.value));
    
    let totalProb = 0;
    let scenarios = 0;
    let totalMatchCount = 0;
    let totalPossibleCount = 0;
    
    for (const attack of attackCards) {
        const possibleCovers: Card[] = [];
        for (let suit = 0; suit < 4; suit++) {
            for (let value = 5; value <= 13; value++) {
                const cardKey = `${suit}-${value}`;
                if (canCover(attack, { suit, value }, game.power_suit) && !isCardFullyAccountedFor(cardKey, tracker)) {
                    possibleCovers.push({ suit, value });
                }
            }
        }
        
        let allowsAttackCount = 0;
        for (const cover of possibleCovers) {
            if (passerValues.has(cover.value) && !tableValues.has(cover.value)) {
                allowsAttackCount++;
            }
        }
        
        totalMatchCount += allowsAttackCount;
        totalPossibleCount += possibleCovers.length;
        
        if (possibleCovers.length > 0) {
            totalProb += allowsAttackCount / possibleCovers.length;
            scenarios++;
        }
    }
    
    return { 
        prob: scenarios > 0 ? totalProb / scenarios : 0,
        matchCount: totalMatchCount,
        totalCount: totalPossibleCount
    };
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * Hypergeometric probability of drawing 0 successes
 * P(X=0) where X ~ Hypergeometric(N, K, n)
 * N = population size (total unknown cards)
 * K = number of success states (cards that match)
 * n = number of draws (cards in hand)
 */
function hypergeometricProbZero(N: number, K: number, n: number): number {
    if (N <= 0 || n <= 0) return 1;
    if (K <= 0) return 1;
    if (K >= N) return 0;
    if (n > N) return 0;
    // If there aren't enough non-success cards to fill the draw, P(zero successes) = 0
    if (N - K < n) return 0;
    
    // P(X=0) = C(N-K, n) / C(N, n)
    // = (N-K)! / (n! * (N-K-n)!) * (n! * (N-n)!) / N!
    // = (N-K)! * (N-n)! / ((N-K-n)! * N!)
    
    // Use log for numerical stability
    let logProb = 0;
    
    // (N-K choose n) / (N choose n)
    for (let i = 0; i < n; i++) {
        logProb += Math.log(N - K - i) - Math.log(N - i);
    }
    
    return Math.exp(logProb);
}

// ============================================================================
// TEST CODE - Remove after verification
// ============================================================================

if (import.meta.url === `file://${process.argv[1]}`) {
    console.log('\n=== Testing DP with Known Defender Cards ===\n');
    
    // Scenario: Defender has 7 cards, we know 3 of them
    // Attack with 2 cards: 8♣ and 8♠
    // Known cards:
    //   - 9♣ (can cover 8♣ only)
    //   - 9♠ (can cover 8♠ only)
    //   - 6♦ (useless, can't cover either)
    // Trump: Hearts (♥)
    // Suit mapping: 0=♠, 1=♥, 2=♣, 3=♦
    
    const attacks: Card[] = [
        { suit: 2, value: 7 }, // 8♣ (suit 2=CLUBS, value 7=8)
        { suit: 0, value: 7 }, // 8♠ (suit 0=SPADES, value 7=8)
        {suit: 1, value: 12},
        {suit: 1, value: 8}
    ];
    
    const knownDefenderCards: Card[] = [
        { suit: 2, value: 8 },  // 9♣ (suit 2=CLUBS, value 8=9) - can cover 8♣
        { suit: 0, value: 8 },  // 9♠ (suit 0=SPADES, value 8=9) - can cover 8♠
        { suit: 3, value: 5 },  // 6♦ (suit 3=DIAMONDS, value 5=6) - useless
    ];
    
    const trumpSuit = 1; // Hearts (suit 1=HEARTS)
    const defenderHandSize = 7; // Total hand size
    const totalUnknownCards = 20; // Unknown cards in deck + other players
    
    // Build excluded cards set (cards NOT in unknown pool: attacks + known defender cards)
    const excludedCards = new Set<string>();
    for (const card of knownDefenderCards) {
        excludedCards.add(cardDisplay(card));
    }
    for (const card of attacks) {
        excludedCards.add(cardDisplay(card));
    }
    
    console.log('Attack cards:', attacks.map(c => cardDisplay(c)).join(', '));
    console.log('Known defender cards:', knownDefenderCards.map(c => cardDisplay(c)).join(', '));
    console.log('Trump suit: Hearts');
    console.log('Defender total hand size:', defenderHandSize);
    console.log('Defender known cards:', knownDefenderCards.length);
    console.log('Defender unknown cards:', defenderHandSize - knownDefenderCards.length);
    console.log('Total unknown cards in pool:', totalUnknownCards);
    console.log('');
    
    // Test WITHOUT known cards
    console.log('--- Test 1: WITHOUT known cards (baseline) ---');
    const probWithout = calculateCoverProbabilityDP(
        attacks,
        excludedCards,
        trumpSuit,
        defenderHandSize,
        totalUnknownCards + knownDefenderCards.length, // Add known cards back to pool
        [] // No known cards
    );
    console.log(`P(Cover) = ${(probWithout * 100).toFixed(2)}%`);
    console.log('');
    
    // Test WITH known cards
    console.log('--- Test 2: WITH known cards ---');
    console.log('Known cards can cover:');
    for (const kCard of knownDefenderCards) {
        const covers = attacks.filter(a => canCover(a, kCard, trumpSuit));
        console.log(`  ${cardDisplay(kCard)} can cover: ${covers.map(c => cardDisplay(c)).join(', ') || 'none'}`);
    }
    const probWith = calculateCoverProbabilityDP(
        attacks,
        excludedCards,
        trumpSuit,
        defenderHandSize,
        totalUnknownCards,
        knownDefenderCards
    );
    console.log(`P(Cover) = ${(probWith * 100).toFixed(2)}%`);
    console.log('');
    
    // Expected result: With known cards covering 2/3 attacks,
    // need to draw A♥ for the K♥, so ~13.33% (4 out of 30)
    console.log('Expected: ~13% since known cards cover 8♣ and 8♠, but need A♥ for K♥');
    console.log('Actual:', `${(probWith * 100).toFixed(2)}%`);
    console.log('');
    
    // Test edge case: All known cards are useless
    console.log('--- Test 3: All known cards are useless ---');
    const uselessKnownCards: Card[] = [
        { suit: 3, value: 5 },  // 6♦ (suit 3=DIAMONDS, value 5=6)
        { suit: 3, value: 6 },  // 7♦ (suit 3=DIAMONDS, value 6=7)
        { suit: 2, value: 5 },  
        { suit: 0, value: 5 },  
        //{ suit: 2, value: 6 },  
    ];
    const probUseless = calculateCoverProbabilityDP(
        attacks,
        excludedCards,
        trumpSuit,
        defenderHandSize,
        totalUnknownCards,
        uselessKnownCards
    );
    console.log('Known cards:', uselessKnownCards.map(c => cardDisplay(c)).join(', '), '(all useless)');
    console.log(`P(Cover) = ${(probUseless * 100).toFixed(2)}%`);
    console.log('');
    
    console.log('=== Tests Complete ===\n');
    
    // ========================================================================
    // TEST: CoverWithGoodRanks
    // ========================================================================
    console.log('\n=== Testing DP with Good Rank Tracking ===\n');
    
    // Scenario: Attacker plays 8♣, 8♠. Their remaining hand has Q♣, A♠, 6♦
    // Good ranks = {Q (value 11), A (value 13), 6 (value 5)}
    // Question: Given defender can cover, what's the chance they use a card
    // with rank Q, A, or 6?
    
    const goodRankAttacks: Card[] = [
        { suit: 2, value: 7 },  // 8♣
        { suit: 0, value: 7 },  // 8♠
    ];
    
    const attackerRemainingHand: Card[] = [
        { suit: 2, value: 11 }, // Q♣
        { suit: 0, value: 13 }, // A♠
        { suit: 3, value: 5 },  // 6♦
    ];
    
    const goodRankExcluded = new Set<string>();
    // Exclude attacks
    for (const card of goodRankAttacks) {
        goodRankExcluded.add(cardDisplay(card));
    }
    // Exclude attacker's remaining hand
    for (const card of attackerRemainingHand) {
        goodRankExcluded.add(cardDisplay(card));
    }
    
    console.log('Attacks:', goodRankAttacks.map(cardDisplay).join(', '));
    console.log('Attacker remaining hand:', attackerRemainingHand.map(cardDisplay).join(', '));
    console.log('Good ranks: Q, A, 6');
    console.log('Trump suit: Hearts');
    console.log('');
    
    const goodRankResult = calculateCoverProbabilityDPWithGoodRanks(
        goodRankAttacks,
        goodRankExcluded,
        1, // Hearts
        6, // Defender hand size
        attackerRemainingHand,
        [], // No known defender cards
        true // Debug mode
    );
    
    console.log('');
    console.log('=== Results ===');
    console.log(`P(Cover) = ${(goodRankResult.pCover * 100).toFixed(2)}%`);
    console.log(`P(∃ cover using Q/A/6 | cover) = ${(goodRankResult.pPossibleGoodRank * 100).toFixed(2)}%`);
    console.log(`P(every cover must use Q/A/6 | cover) = ${(goodRankResult.pForcedGoodRank * 100).toFixed(2)}%`);
    console.log('');
    
    // Interpretation:
    // - pPossibleGoodRank: If they can cover, chance there EXISTS a way using a matching rank
    // - pForcedGoodRank: If they can cover, chance ALL their covering options use matching ranks
    
    console.log('=== Good Rank Tests Complete ===\n');
    
    // ========================================================================
    // VALIDATION TEST: Known defender cards with good ranks
    // ========================================================================
    console.log('\n=== VALIDATION: Known Defender Cards Start State ===\n');
    
    // Realistic example:
    // 4 attacks: 8♣, 8♠, 8♦, 8♥
    // Defender hand size: 7
    // Known defender cards (3):
    //   - J♥ (trump): covers ALL 4 attacks (mask=1111), not a good rank
    //   - 9♣: covers only 8♣ (mask=0001), not a good rank  
    //   - 6♦: useless, covers nothing (mask=0000), not a good rank
    // Unknown slots to fill: 7 - 3 = 4
    // 
    // Good ranks: Q, K (from attacker's remaining hand Q♣, K♠)
    //
    // Expected A0_init calculation:
    // Start: A0 = {∅}, A1 = ∅
    // After J♥ (mask=1111, g=0): A0 = {∅,{0},{1},{2},{3},{0,1},{0,2},{0,3},{1,2},{1,3},{2,3},{0,1,2},{0,1,3},{0,2,3},{1,2,3},{0,1,2,3}}, A1 = ∅
    //   (all 16 subsets achievable since J♥ can cover any single attack)
    // After 9♣ (mask=0001, g=0): stays same (9♣ only adds attack 0, already covered by J♥)
    // After 6♦ (mask=0000, g=0): stays same (useless card)
    // 
    // So A0_init = 65535 (all 16 bits set), A1_init = 0
    
    const validationAttacks: Card[] = [
        { suit: 2, value: 7 },  // 8♣ (attack 0)
        { suit: 0, value: 7 },  // 8♠ (attack 1)
        { suit: 3, value: 7 },  // 8♦ (attack 2)
        { suit: 1, value: 7 },  // 8♥ (attack 3)
    ];
    
    const validationAttackerRemaining: Card[] = [
        { suit: 2, value: 11 }, // Q♣ - good rank Q
        { suit: 0, value: 12 }, // K♠ - good rank K
    ];
    
    const validationKnownDefender: Card[] = [
        { suit: 1, value: 10 }, // J♥ (trump) - covers ALL attacks
        { suit: 2, value: 8 },  // 9♣ - covers only 8♣
        { suit: 3, value: 5 },  // 6♦ - useless
    ];
    
    const validationExcluded = new Set<string>();
    for (const card of validationAttacks) validationExcluded.add(cardDisplay(card));
    for (const card of validationAttackerRemaining) validationExcluded.add(cardDisplay(card));
    for (const card of validationKnownDefender) validationExcluded.add(cardDisplay(card));
    
    console.log('Attacks (4):', validationAttacks.map(cardDisplay).join(', '));
    console.log('Attacker remaining hand:', validationAttackerRemaining.map(cardDisplay).join(', '));
    console.log('Good ranks: Q, K');
    console.log('');
    console.log('Known defender cards (3):');
    console.log('  J♥ (trump) - can cover ALL attacks (mask=1111)');
    console.log('  9♣ - can cover only 8♣ (mask=0001)');
    console.log('  6♦ - useless (mask=0000)');
    console.log('');
    console.log('Defender total hand size: 7');
    console.log('Known cards: 3');
    console.log('Unknown slots to fill: 4');
    console.log('');
    
    // Verify with initializeDPWithKnownCardsPair
    const goodRanksValidation = new Set<number>([11, 12]); // Q, K
    const subsetCountValidation = 1 << validationAttacks.length; // 16
    const dpInit = initializeDPWithKnownCardsPair(
        validationKnownDefender,
        validationAttacks,
        1, // Hearts trump
        goodRanksValidation,
        subsetCountValidation
    );
    
    console.log('Manual calculation of A0_init, A1_init:');
    console.log('  Start: A0={∅}, A1=∅');
    console.log('  After J♥ (mask=1111, g=0): A0={all 16 subsets}, A1=∅');
    console.log('  After 9♣ (mask=0001, g=0): no change');
    console.log('  After 6♦ (mask=0000, g=0): no change');
    console.log('  Expected: A0=65535 (all subsets), A1=0');
    console.log('');
    
    console.log('Actual DP initialization:');
    for (const [r, states] of Object.entries(dpInit)) {
        for (const [stateStr, ways] of Object.entries(states)) {
            const [A0Str, A1Str] = stateStr.split('|');
            const A0 = BigInt(A0Str);
            const A1 = BigInt(A1Str);
            
            // Count bits set
            let a0Count = 0, a1Count = 0;
            for (let i = 0; i < 16; i++) {
                if ((A0 & (1n << BigInt(i))) !== 0n) a0Count++;
                if ((A1 & (1n << BigInt(i))) !== 0n) a1Count++;
            }
            
            console.log(`  dp[${r}]: A0=${A0Str} (${a0Count} subsets), A1=${A1Str} (${a1Count} subsets)`);
            
            // Check if FULL is achievable
            const fullSetBit = 1n << BigInt(15); // {0,1,2,3}
            const a0HasFull = (A0 & fullSetBit) !== 0n;
            const a1HasFull = (A1 & fullSetBit) !== 0n;
            console.log(`  A0 has FULL: ${a0HasFull}, A1 has FULL: ${a1HasFull}`);
        }
    }
    console.log('');
    
    // Now run full calculation
    console.log('--- Full calculation with 4 unknown slots to fill ---');
    const validationResult = calculateCoverProbabilityDPWithGoodRanks(
        validationAttacks,
        validationExcluded,
        1, // Hearts trump
        7, // Defender has 7 cards total
        validationAttackerRemaining,
        validationKnownDefender,
        true // Debug mode
    );
    
    console.log('');
    console.log('=== Validation Results ===');
    console.log(`P(Cover) = ${(validationResult.pCover * 100).toFixed(2)}%`);
    console.log(`P(∃ cover using Q/K | cover) = ${(validationResult.pPossibleGoodRank * 100).toFixed(2)}%`);
    console.log(`P(every cover must use Q/K | cover) = ${(validationResult.pForcedGoodRank * 100).toFixed(2)}%`);
    console.log('');
    console.log('Interpretation:');
    console.log('  - IMPORTANT: Each card can only cover ONE attack!');
    console.log('  - J♥ can cover ANY one of the 4 attacks, but only ONE');
    console.log('  - 9♣ can cover attack 0 (8♣) only');
    console.log('  - 6♦ is useless');
    console.log('  - So with 3 known cards, max coverage = 2 attacks');
    console.log('  - Need 2+ more cards from unknown pool to cover remaining attacks');
    console.log('  - Unknown slots to fill: 4 (defender hand=7, known=3)');
    console.log('  - P(Cover) < 100% because we might not draw enough covers for attacks 1,2,3');
    console.log('');
    
    console.log('=== Validation Complete ===\n');
    
    // ========================================================================
    // TEST: Attack with high trump + regular card - mixed scenario
    // ========================================================================
    console.log('\n=== TEST: High Trump + Regular Attack (Mixed Scenario) ===\n');
    
    // Scenario:
    // Attack with Q♥ (trump) + 8♣
    // Q♥ can ONLY be covered by K♥ or A♥ (both good ranks K, A)
    // 8♣ can be covered by 9♣, 10♣, J♣, Q♣, K♣, A♣ or any trump 6♥-A♥
    // Attacker's remaining hand has K♦, A♦ - good ranks = {K, A}
    //
    // Analysis:
    // - Q♥ MUST use a good rank (K♥ or A♥)
    // - 8♣ can use non-good ranks (9♣, 10♣, J♣, Q♣, or 6♥-J♥)
    // - So P(Forced) < 100% but P(Possible) should be high
    
    const highTrumpAttacks: Card[] = [
        { suit: 1, value: 11 },  // Q♥ (attack 0) - trump queen
        { suit: 2, value: 7 },   // 8♣ (attack 1) - regular card
    ];
    
    const highTrumpAttackerRemaining: Card[] = [
        { suit: 3, value: 12 }, // K♦ - good rank K
        { suit: 3, value: 13 }, // A♦ - good rank A
    ];
    
    const highTrumpExcluded = new Set<string>();
    for (const card of highTrumpAttacks) highTrumpExcluded.add(cardDisplay(card));
    for (const card of highTrumpAttackerRemaining) highTrumpExcluded.add(cardDisplay(card));
    
    console.log('Attacks:', highTrumpAttacks.map(cardDisplay).join(', '));
    console.log('Trump suit: Hearts');
    console.log('Attacker remaining hand:', highTrumpAttackerRemaining.map(cardDisplay).join(', '));
    console.log('Good ranks: K, A');
    console.log('');
    console.log('Analysis:');
    console.log('  Q♥ can ONLY be covered by: K♥ or A♥ (both good ranks)');
    console.log('  8♣ can be covered by: 9♣-A♣ or any trump (6♥-A♥)');
    console.log('  To cover both, defender MUST use K♥ or A♥ for Q♥');
    console.log('  But 8♣ can be covered with non-good ranks (9♣, 10♣, J♣, Q♣, 6♥-J♥)');
    console.log('  P(Forced) = chance they have NO non-good-rank covers for 8♣');
    console.log('');
    
    const highTrumpResult = calculateCoverProbabilityDPWithGoodRanks(
        highTrumpAttacks,
        highTrumpExcluded,
        1, // Hearts trump
        6, // Defender hand size
        highTrumpAttackerRemaining,
        [], // No known defender cards
        true // Debug mode
    );
    
    console.log('');
    console.log('=== High Trump + Regular Results ===');
    console.log(`P(Cover) = ${(highTrumpResult.pCover * 100).toFixed(2)}%`);
    console.log(`P(∃ cover using K/A | cover) = ${(highTrumpResult.pPossibleGoodRank * 100).toFixed(2)}%`);
    console.log(`P(every cover must use K/A | cover) = ${(highTrumpResult.pForcedGoodRank * 100).toFixed(2)}%`);
    console.log('');
    console.log('Expected: P(Possible) = 100% (must use K/A for Q♥) ✓');
    console.log('Expected: P(Forced) = 100% (Q♥ requires K/A, so every cover uses K/A)');
    console.log('Note: Even though 8♣ CAN use non-K/A, Q♥ FORCES use of K/A');
    console.log(`Actual P(Forced) = ${(highTrumpResult.pForcedGoodRank * 100).toFixed(2)}% ✓`);
    console.log('');
    
    // ========================================================================
    // TEST: Same scenario WITH known defender cards
    // ========================================================================
    console.log('\n=== TEST: High Trump + Regular WITH Known Defender Cards ===\n');
    
    // Same attacks (Q♥ + 8♣), but now we know some defender cards:
    // - K♥ (covers Q♥, IS a good rank K)
    // - 9♣ (covers 8♣, NOT a good rank)
    // - 6♦ (useless)
    //
    // Analysis:
    // - K♥ handles Q♥ (good rank used)
    // - 9♣ handles 8♣ (non-good rank used)
    // - So there EXISTS a cover that uses only 1 good-rank card
    // - P(Cover) = 100%
    // - P(Forced) < 100% because 9♣ covers 8♣ without using K/A
    
    const highTrumpKnownDefender: Card[] = [
        { suit: 1, value: 12 }, // K♥ - covers Q♥, IS a good rank
        { suit: 2, value: 8 },  // 9♣ - covers 8♣, NOT a good rank
        { suit: 3, value: 5 },  // 6♦ - useless
    ];
    
    // Update excluded to include known defender cards
    const highTrumpExcluded2 = new Set<string>();
    for (const card of highTrumpAttacks) highTrumpExcluded2.add(cardDisplay(card));
    for (const card of highTrumpAttackerRemaining) highTrumpExcluded2.add(cardDisplay(card));
    for (const card of highTrumpKnownDefender) highTrumpExcluded2.add(cardDisplay(card));
    
    console.log('Attacks:', highTrumpAttacks.map(cardDisplay).join(', '));
    console.log('Attacker remaining hand:', highTrumpAttackerRemaining.map(cardDisplay).join(', '));
    console.log('Good ranks: K, A');
    console.log('');
    console.log('Known defender cards:');
    console.log('  K♥ - covers Q♥, IS a good rank (K)');
    console.log('  9♣ - covers 8♣, NOT a good rank');
    console.log('  6♦ - useless');
    console.log('');
    console.log('Analysis:');
    console.log('  K♥ covers Q♥ (good rank) + 9♣ covers 8♣ (non-good rank)');
    console.log('  P(Cover) = 100% (both attacks covered by known cards)');
    console.log('  A0_init should have {}, {1} (9♣ can cover 8♣ without good rank)');
    console.log('  A1_init should have {0}, {1}, {0,1} (K♥ covers Q♥, can also help with 8♣)');
    console.log('  KEY: {0,1} is in A1 only, not A0 (Q♥ requires K♥ which is good rank)');
    console.log('  P(Forced) = 100% because EVERY cover of Q♥ uses K♥ or A♥!');
    console.log('');
    
    // Verify initialization
    const goodRanksHighTrump = new Set<number>([12, 13]); // K, A
    const subsetCountHighTrump = 1 << highTrumpAttacks.length; // 4 (2 attacks)
    const dpInitHighTrump = initializeDPWithKnownCardsPair(
        highTrumpKnownDefender,
        highTrumpAttacks,
        1, // Hearts trump
        goodRanksHighTrump,
        subsetCountHighTrump
    );
    
    console.log('DP Initialization with known cards:');
    for (const [r, states] of Object.entries(dpInitHighTrump)) {
        for (const [stateStr, ways] of Object.entries(states)) {
            const [A0Str, A1Str] = stateStr.split('|');
            const A0 = BigInt(A0Str);
            const A1 = BigInt(A1Str);
            
            // Decode subsets for 2 attacks
            const a0Sets: string[] = [];
            const a1Sets: string[] = [];
            for (let s = 0; s < 4; s++) {
                if ((A0 & (1n << BigInt(s))) !== 0n) {
                    const indices: number[] = [];
                    if (s & 1) indices.push(0);
                    if (s & 2) indices.push(1);
                    a0Sets.push(indices.length === 0 ? '{}' : `{${indices.join(',')}}`);
                }
                if ((A1 & (1n << BigInt(s))) !== 0n) {
                    const indices: number[] = [];
                    if (s & 1) indices.push(0);
                    if (s & 2) indices.push(1);
                    a1Sets.push(indices.length === 0 ? '{}' : `{${indices.join(',')}}`);
                }
            }
            
            console.log(`  dp[${r}]: A0={${a0Sets.join(',')}}, A1={${a1Sets.join(',')}}`);
            const fullSetBit = 1n << BigInt(3); // {0,1} = bit 3
            console.log(`  A0 has FULL: ${(A0 & fullSetBit) !== 0n}, A1 has FULL: ${(A1 & fullSetBit) !== 0n}`);
        }
    }
    console.log('');
    
    const highTrumpResult2 = calculateCoverProbabilityDPWithGoodRanks(
        highTrumpAttacks,
        highTrumpExcluded2,
        1, // Hearts trump
        6, // Defender hand size
        highTrumpAttackerRemaining,
        highTrumpKnownDefender,
        true // Debug mode
    );
    
    console.log('');
    console.log('=== High Trump + Regular WITH Known Cards Results ===');
    console.log(`P(Cover) = ${(highTrumpResult2.pCover * 100).toFixed(2)}%`);
    console.log(`P(∃ cover using K/A | cover) = ${(highTrumpResult2.pPossibleGoodRank * 100).toFixed(2)}%`);
    console.log(`P(every cover must use K/A | cover) = ${(highTrumpResult2.pForcedGoodRank * 100).toFixed(2)}%`);
    console.log('');
    console.log(`Expected P(Cover) = 100.00% - ${highTrumpResult2.pCover === 1 ? '✓ CORRECT' : '✗ WRONG'}`);
    console.log(`Expected P(Forced) = 100.00% - ${highTrumpResult2.pForcedGoodRank === 1 ? '✓ CORRECT' : '✗ WRONG'}`);
    console.log('Note: P(Forced)=100% because Q♥ requires K/A, regardless of 9♣ for 8♣');
    console.log('');
    
    // ========================================================================
    // TEST: Known defender has A♥ but NO cover for 8♣
    // ========================================================================
    console.log('\n=== TEST: High Trump + Regular - Known A♥ Only ===\n');
    
    // Same attacks (Q♥ + 8♣), known cards:
    // - A♥ (covers Q♥, IS a good rank A)
    // - 7♦ (useless)
    // - 6♠ (useless)
    //
    // Analysis:
    // - A♥ handles Q♥ (good rank used)
    // - 8♣ needs a cover from unknown pool
    // - P(Cover) depends on drawing a cover for 8♣
    // - P(Forced) depends on whether that cover is K/A
    
    const highTrumpKnownDefender3: Card[] = [
        { suit: 1, value: 13 }, // A♥ - covers Q♥, IS a good rank (A)
        { suit: 3, value: 6 },  // 7♦ - useless
        { suit: 0, value: 5 },  // 6♠ - useless
    ];
    
    const highTrumpExcluded3 = new Set<string>();
    for (const card of highTrumpAttacks) highTrumpExcluded3.add(cardDisplay(card));
    for (const card of highTrumpAttackerRemaining) highTrumpExcluded3.add(cardDisplay(card));
    for (const card of highTrumpKnownDefender3) highTrumpExcluded3.add(cardDisplay(card));
    
    console.log('Attacks:', highTrumpAttacks.map(cardDisplay).join(', '));
    console.log('Attacker remaining hand:', highTrumpAttackerRemaining.map(cardDisplay).join(', '));
    console.log('Good ranks: K, A');
    console.log('');
    console.log('Known defender cards:');
    console.log('  A♥ - covers Q♥, IS a good rank (A)');
    console.log('  7♦ - useless');
    console.log('  6♠ - useless');
    console.log('');
    console.log('Analysis:');
    console.log('  A♥ covers Q♥ (good rank used) - attack 0 DONE');
    console.log('  8♣ needs cover from unknown pool');
    console.log('  Covers for 8♣: 9♣-A♣ or 6♥-K♥ (not A♥, already used)');
    console.log('  P(Cover) < 100% (need to draw a cover for 8♣)');
    console.log('  P(Possible) = 100% (A♥ already used for Q♥)');
    console.log('  P(Forced) = low (many non-K/A covers for 8♣: 9♣,10♣,J♣,Q♣,6♥-J♥)');
    console.log('');
    
    const highTrumpResult3 = calculateCoverProbabilityDPWithGoodRanks(
        highTrumpAttacks,
        highTrumpExcluded3,
        1, // Hearts trump
        6, // Defender hand size
        highTrumpAttackerRemaining,
        highTrumpKnownDefender3,
        true // Debug mode
    );
    
    console.log('');
    console.log('=== Results ===');
    console.log(`P(Cover) = ${(highTrumpResult3.pCover * 100).toFixed(2)}%`);
    console.log(`P(∃ cover using K/A | cover) = ${(highTrumpResult3.pPossibleGoodRank * 100).toFixed(2)}%`);
    console.log(`P(every cover must use K/A | cover) = ${(highTrumpResult3.pForcedGoodRank * 100).toFixed(2)}%`);
    console.log('');
    console.log('Expected:');
    console.log('  P(Possible) = 100% (A♥ already covers Q♥) ✓');
    console.log('  P(Forced) = 100% (Q♥ requires A♥ or K♥, both are good ranks)');
    console.log('Note: Even though 8♣ CAN use non-K/A, Q♥ FORCES use of K/A');
    console.log('');
    
    console.log('=== All High Trump Tests Complete ===\n');
    
    // ========================================================================
    // TEST 4: Low cards that DON'T force good-rank usage
    // ========================================================================
    console.log('\n=== TEST 4: Low Cards (P(Forced) < 100%) ===\n');
    
    // Scenario:
    // Attacks: 8♣, 8♠ (both can be covered by many non-good-rank cards)
    // Good ranks: Q, K (attacker has Q♦, K♦)
    //
    // Covers for 8♣: 9♣-A♣ + any trump
    // Covers for 8♠: 9♠-A♠ + any trump
    // Non-Q/K covers exist: 9♣,10♣,J♣,A♣ for 8♣; 9♠,10♠,J♠,A♠ for 8♠; + most trumps
    //
    // So there EXIST valid covers using NO Q or K → P(Forced) < 100%
    
    const lowAttacks: Card[] = [
        { suit: 2, value: 7 },  // 8♣ (attack 0)
        { suit: 0, value: 7 },  // 8♠ (attack 1)
    ];
    
    const lowAttackerRemaining: Card[] = [
        { suit: 3, value: 11 }, // Q♦ - good rank Q
        { suit: 3, value: 12 }, // K♦ - good rank K
    ];
    
    const lowExcluded = new Set<string>();
    for (const card of lowAttacks) lowExcluded.add(cardDisplay(card));
    for (const card of lowAttackerRemaining) lowExcluded.add(cardDisplay(card));
    
    console.log('Attacks:', lowAttacks.map(cardDisplay).join(', '));
    console.log('Trump suit: Hearts');
    console.log('Attacker remaining hand:', lowAttackerRemaining.map(cardDisplay).join(', '));
    console.log('Good ranks: Q, K');
    console.log('');
    console.log('Analysis:');
    console.log('  8♣ can be covered by: 9♣-A♣ or 6♥-A♥');
    console.log('  8♠ can be covered by: 9♠-A♠ or 6♥-A♥');
    console.log('  Non-Q/K covers for 8♣: 9♣,10♣,J♣,A♣ + 6♥-J♥,A♥ (10 cards)');
    console.log('  Non-Q/K covers for 8♠: 9♠,10♠,J♠,A♠ + 6♥-J♥,A♥ (10 cards)');
    console.log('  → EXIST valid covers using NO Q or K');
    console.log('  → P(Forced) should be < 100%!');
    console.log('');
    
    const lowResult = calculateCoverProbabilityDPWithGoodRanks(
        lowAttacks,
        lowExcluded,
        1, // Hearts trump
        6, // Defender hand size
        lowAttackerRemaining,
        [], // No known defender cards
        true // Debug mode
    );
    
    console.log('');
    console.log('=== Low Cards Results ===');
    console.log(`P(Cover) = ${(lowResult.pCover * 100).toFixed(2)}%`);
    console.log(`P(∃ cover using Q/K | cover) = ${(lowResult.pPossibleGoodRank * 100).toFixed(2)}%`);
    console.log(`P(every cover must use Q/K | cover) = ${(lowResult.pForcedGoodRank * 100).toFixed(2)}%`);
    console.log('');
    console.log(`P(Forced) < 100%? ${lowResult.pForcedGoodRank < 1 ? '✓ YES - as expected!' : '✗ NO - check logic'}`);
    console.log('');
    
    // ========================================================================
    // PASS PROBABILITY TEST - 4 Player Scenario
    // ========================================================================
    console.log('\n=== PASS PROBABILITY TEST: 4 Players ===\n');
    
    // Scenario:
    // - 4 players: Us (0), Player1 (1), Player2 (2), Player3 (3)
    // - Player1 (to our left) is the attacker
    // - Player2 (2 seats to our right) is the defender
    // - Attack: 8 of Clubs
    // - We have: 8 of Diamonds in our hand (plus 5 other cards)
    // - Flipped card: J of Spades
    // - No discard yet
    //
    // For pass to come back to Player1 (attacker):
    // - Player2 (defender) passes to Player3
    // - Player3 passes to Us (position 0)
    // - We pass to Player1 (attacker)
    // Wait, that's 3 passers! Let me reconsider...
    //
    // Actually:
    // - Player1 attacks Player2
    // - For pass to come back to Player1, the chain is:
    //   Player2 → Player3 → Us → (back to Player1)
    // But we're calculating if THEY can pass back to US (the attacker is to our left)
    // So if WE attack, the defender would be Player2, and pass chain is Player2 → Player3 → back to us
    // That's d=2 passers (Player2 and Player3)
    //
    // Let me set up correctly per user's description:
    // - Player to our left is the attacker (position 1)
    // - Player 2 seats to our right is under attack (that's position 2 in a 4-player game)
    // - Pass chain from defender back to attacker: Player2 → Player3 → back to Player1
    // - d = 2 (Player2 and Player3)
    
    console.log('Scenario Setup:');
    console.log('  4 players: Us (0), Player1 (1), Player2 (2), Player3 (3)');
    console.log('  WE (position 0) attack Player2 (2 seats to our right, position 2)');
    console.log('  Attack: 8♣');
    console.log('  We have 8♦ in our hand (so we know one 8 is accounted for)');
    console.log('  Flipped: J♠');
    console.log('');
    console.log('For pass back to US (attacker):');
    console.log('  Pass chain: Player2 → Player3 → Us');
    console.log('  Distance d = 2 (Player2 and Player3 need to pass)');
    console.log('  Attack size a = 1');
    console.log('  R = 4 - 1 = 3 remaining 8s (formula)');
    console.log('  But: 8♦ in our hand → only 2 unknown 8s (8♥, 8♠)');
    console.log('');
    
    // Known cards: 6 (our hand) + 1 (table) + 1 (flipped) = 8
    // Unknown: 36 - 8 = 28
    // But R is only 2 (we have one 8, one is on table)
    
    const passTestGame: Game = {
        id: 'pass_test',
        name: 'Pass Test',
        status: GAME_STATUS.PLAYING,
        deck: Array(17).fill(null), // 17 cards in deck
        deck_length: 17,
        discard_pile_length: 0,
        flipped: { suit: 0, value: 10 }, // J♠
        power_suit: 0, // Spades
        first_attacker: 0, // Us
        defender: 2, // Player2 (2 seats to our right)
        table_battles: [{ attack: { suit: 2, value: 7 }, defense: null }], // 8♣ on table
        elimination_order: [],
        good_timestamp: null,
        good_players: [],
        logs: [],
        players: [
            { // Us - position 0 (attacker)
                player_id: 'us',
                name: 'Us',
                status: PLAYER_STATUS.READY,
                is_ai: true,
                hand: [
                    { suit: 3, value: 7 },  // 8♦ - we have this!
                    { suit: 0, value: 5 },  // 6♠
                    { suit: 1, value: 9 },  // 10♥
                    { suit: 2, value: 11 }, // Q♣
                    { suit: 3, value: 12 }, // K♦
                    // Used 8♣ for attack, so 5 cards left
                ],
                awaiting_attack: false,
                hand_length: 5,
                strategy_key: STRATEGY_KEY.RANDOM,
            },
            { // Player1 - position 1 (to our left)
                player_id: 'player1',
                name: 'Player1',
                status: PLAYER_STATUS.READY,
                is_ai: true,
                hand: Array(6).fill({ suit: 0, value: 5 }), // 6 cards
                awaiting_attack: false,
                hand_length: 6,
                strategy_key: STRATEGY_KEY.RANDOM,
            },
            { // Player2 - position 2 (defender, 2 seats to our right)
                player_id: 'player2',
                name: 'Player2',
                status: PLAYER_STATUS.READY,
                is_ai: true,
                hand: Array(6).fill({ suit: 0, value: 5 }), // 6 cards
                awaiting_attack: false,
                hand_length: 6,
                strategy_key: STRATEGY_KEY.RANDOM,
            },
            { // Player3 - position 3 (1 seat to our right)
                player_id: 'player3',
                name: 'Player3',
                status: PLAYER_STATUS.READY,
                is_ai: true,
                hand: Array(6).fill({ suit: 0, value: 5 }), // 6 cards
                awaiting_attack: false,
                hand_length: 6,
                strategy_key: STRATEGY_KEY.RANDOM,
            },
        ],
    };
    
    // Create tracker from our perspective
    const passTestTracker = new CardTracker(passTestGame, 'us');
    
    console.log('Card Tracking:');
    console.log(`  Our hand: 5 cards (including 8♦, we used 8♣ for attack)`);
    console.log(`  On table: 8♣`);
    console.log(`  Flipped: J♠`);
    console.log(`  Other players: 6 + 6 + 6 = 18 cards`);
    console.log('');
    
    // 36 cards total in Durak
    // Our hand: 5, Player1: 6, Player2: 6, Player3: 6 = 23 in hands
    // Table: 1
    // Deck: 36 - 23 - 1 = 12 cards (flipped is part of deck but visible)
    
    console.log('Corrected count:');
    console.log(`  All hands: 5 + 6 + 6 + 6 = 23 cards`);
    console.log(`  Table: 1 card`);
    console.log(`  Deck: 12 cards (36 - 23 - 1 = 12)`);
    console.log(`  Flipped: 1 (part of deck, but visible)`);
    console.log('');
    
    // Fix deck size
    passTestGame.deck = Array(12).fill(null);
    passTestGame.deck_length = 12;
    
    // Calculate N (unknown cards from our perspective)
    // N = opponent hands + deck - known opponent cards
    // = (6 + 6 + 6) + 12 - 0 = 30
    // But we know flipped, so: N = 18 + 12 - 1 = 29
    
    console.log('Unknown cards (N):');
    console.log(`  Opponent hands: 6 + 6 + 6 = 18`);
    console.log(`  Deck: 12`);
    console.log(`  Minus flipped (visible): -1`);
    console.log(`  N = 18 + 12 - 1 = 29`);
    console.log('');
    
    // R = remaining 8s that are unknown
    // 4 total 8s: 8♣ (table), 8♦ (our hand), 8♥ (unknown), 8♠ (unknown)
    // R = 2
    
    console.log('Remaining 8s (R):');
    console.log(`  Total 8s: 4`);
    console.log(`  On table: 8♣`);
    console.log(`  In our hand: 8♦`);
    console.log(`  Unknown: 8♥, 8♠`);
    console.log(`  R = 2`);
    console.log('');
    
    // Calculate pass probability
    // d = 2 passers, a = 1 attack, R = 2 unknown copies
    // Formula: P(2, a=1, R=2) with N=28, h1=6, h2=6
    
    // Actually wait - the expert formula uses R = 4 - a, not the actual unknown copies
    // But we need to adjust because we KNOW some copies aren't available
    // The preCheckPassWithTracker handles this with effectiveR = copiesUnknown + copiesInPasserHands
    
    // Let's use the actual function
    const attackValue = 7; // 8 in internal representation (6=5, 7=6, 8=7)
    const attackSize = 1;
    // Pass chain from defender back to us: Player2 → Player3 → Us
    // Passers are Player2 and Player3 (we're the attacker, so we're excluded)
    const passerIds = ['player2', 'player3']; // Defender and next player
    
    const preCheckResult = preCheckPassWithTracker(
        passTestGame,
        passTestTracker,
        attackValue,
        attackSize,
        passerIds,
        'us' // Our perspective (we're the attacker)
    );
    
    console.log('Pre-check Result:');
    console.log(`  isDefinitelyPossible: ${preCheckResult.isDefinitelyPossible}`);
    console.log(`  isDefinitelyImpossible: ${preCheckResult.isDefinitelyImpossible}`);
    console.log(`  reason: ${preCheckResult.reason}`);
    console.log(`  remainingCopies (R=4-a): ${preCheckResult.remainingCopies}`);
    console.log(`  copiesUnknown: ${preCheckResult.copiesUnknown}`);
    console.log(`  copiesInPasserHands: ${preCheckResult.copiesInPasserHands}`);
    console.log(`  copiesAccountedElsewhere: ${preCheckResult.copiesAccountedElsewhere}`);
    console.log('');
    
    // Calculate probability with the formula
    // effectiveR = copiesUnknown (should be 2: 8♥ and 8♠)
    const effectiveR = preCheckResult.copiesUnknown;
    const N = 29; // From our calculation above
    const h1 = 6; // Player2's hand size
    const h2 = 6; // Player3's hand size
    
    // Using inclusion-exclusion: P(2, R) with N=29, h=[6,6]
    const prob = calcPassProbFormula(2, effectiveR, N, [h1, h2]);
    
    console.log('Probability Calculation:');
    console.log(`  d = 2 (passers: Player2, Player3)`);
    console.log(`  effectiveR = ${effectiveR} (unknown 8s, expected: 2)`);
    console.log(`  N = ${N} (unknown cards)`);
    console.log(`  h = [${h1}, ${h2}] (passer hand sizes)`);
    console.log('');
    console.log(`  P(Pass back) = ${(prob * 100).toFixed(2)}%`);
    console.log('');
    
    // Manual verification for R=2, N=29, h=6
    // P(2,a=1,R=2) = 1 - 2*C(N-h,R)/C(N,R) + C(N-2h,R)/C(N,R)
    // C(29,2) = 406, C(23,2) = 253, C(17,2) = 136
    // P = 1 - 2*(253/406) + (136/406) = (406 - 506 + 136)/406 = 36/406 = 8.87%
    
    const C_N_2 = N * (N - 1) / 2;
    const C_Nh_2 = (N - h1) * (N - h1 - 1) / 2;
    const C_N2h_2 = (N - 2 * h1) * (N - 2 * h1 - 1) / 2;
    const manualProb = 1 - 2 * (C_Nh_2 / C_N_2) + (C_N2h_2 / C_N_2);
    
    console.log('Manual Verification (for R=2):');
    console.log(`  C(${N},2) = ${C_N_2}`);
    console.log(`  C(${N - h1},2) = ${C_Nh_2}`);
    console.log(`  C(${N - 2 * h1},2) = ${C_N2h_2}`);
    console.log(`  P = 1 - 2*(${C_Nh_2}/${C_N_2}) + (${C_N2h_2}/${C_N_2})`);
    console.log(`    = ${(manualProb * 100).toFixed(2)}%`);
    console.log('');
    console.log(`  Formula result matches manual (if effectiveR=2): ${Math.abs(prob - manualProb) < 0.001 && effectiveR === 2 ? '✓' : '✗'}`);
    console.log('');
    
    console.log('=== Pass Probability Test Complete ===\n');
    
    console.log('=== All Tests Complete ===\n');
}
