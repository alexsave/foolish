import { Game, Card } from '../../core/types.ts';
import { LegalMove } from '../../core/bot_interfaces.ts';
import { CardTracker } from '../durakai/cardTracker.ts';
import { cardDisplay, getCardValue, get_next_player_index, canCover } from '../common_utils.ts';
import { PLAYER_STATUS } from '../../core/types.ts';
import { SUITS } from '../../core/constants.ts';
import { 
    preCheckPassWithTracker, 
    calculatePassProbability as calcPassProbFormula,
    isPassDefinitelyImpossible 
} from './pass_prob.ts';

// Value names for debugging (value 5 = "6", value 6 = "7", etc.)
const VALUE_NAMES: Record<number, string> = {
    5: '6', 6: '7', 7: '8', 8: '9', 9: '10', 10: 'J', 11: 'Q', 12: 'K', 13: 'A'
};

interface AttackStats {
    canDefinitelyCover: boolean;
    definitelyCannotCover: boolean;
    canDefinitelyPassBack: boolean;
    definitelyCannotPassBack: boolean;
    probCover: number;
    probPass: number;
    probCoverAllowsAttack: number;
}

interface CoverStats {
    probAllowsAdditionalAttack: number;
    probDrawBetterCard: number;
}

interface PassStats {
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
        // Attack: P(Cover), P(CoveringWillAllowAttack), P(PassBackPossible)
        return `      P(Cover): ${pctDef(a.probCover, a.canDefinitelyCover, a.definitelyCannotCover)} | P(CoveringWillAllowAttack): ${pct(a.probCoverAllowsAttack)} | P(PassBackPossible): ${pctDef(a.probPass, a.canDefinitelyPassBack, a.definitelyCannotPassBack)}`;
    }
    
    if (stats.cover) {
        const c = stats.cover;
        // Cover: P(AllowsAtk), P(ForcesPickup), P(DrawBetter)
        return `      P(AllowsAtk): ${pct(c.probAllowsAdditionalAttack)} | P(DrawBetter): ${pct(c.probDrawBetterCard)}`;
    }
    
    if (stats.pass) {
        const p = stats.pass;
        // Pass: P(Cover) for new defender, P(CoveringWillAllowAttack), P(PassBackPossible) to us
        return `      P(Cover): ${pctDef(p.probCover, p.canDefinitelyCover, p.definitelyCannotCover)} | P(CoveringWillAllowAttack): ${pct(p.probCoverAllowsAttack)} | P(PassBackPossible): ${pctDef(p.probPass, p.canDefinitelyPassBack, p.definitelyCannotPassBack)}`;
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

// Debug info interfaces for formula display
export interface DebugInfo {
    attack?: AttackDebugInfo;
    cover?: CoverDebugInfo;
    pass?: PassDebugInfo;
}

interface AttackDebugInfo {
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

interface CoverDebugInfo {
    unknownTotal: number;
    coverValue: number;
    valueCardsRemaining: number;
    attackerUnknownHand: number;
    canCoverValues: string;
    avgUnknownValue: number;
    cardValue: number;
}

interface PassDebugInfo {
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

interface CoverWithGoodRankResult {
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
 * @param minCardValue - Minimum card value in the deck (5 for 36-card deck, 1 for 52-card deck)
 * @param knownDefenderCards - Known defender cards
 * @param debugMode - Enable debug output
 */
function calculateCoverProbabilityDPWithGoodRanks(
    attacks: Card[],
    excludedCards: Set<string>,
    trumpSuit: number,
    defenderHandSize: number,
    attackerRemainingHand: Card[],
    minCardValue: number,
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
        for (let value = minCardValue; value <= ACE_VALUE; value++) {
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
            for (let value = tracker.getMinCardValue(); value <= ACE_VALUE; value++) {
                if (canCover(cards[0], { suit, value }, game.power_suit)) {
                    if (!tracker.isCardAccountedFor(`${suit}-${value}`)) {
                        coverCardsCount++;
                    }
                }
            }
        }
    }
    
    // Count value cards remaining for pass
    let valueCardsRemaining = 0;
    for (let suit = 0; suit < 4; suit++) {
        if (!tracker.isCardAccountedFor(`${suit}-${attackValue}`)) {
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
        for (let value = tracker.getMinCardValue(); value <= ACE_VALUE; value++) {
            if (tracker.isCardAccountedFor(`${suit}-${value}`)) {
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
        tracker.getMinCardValue(),  // min card value
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
            for (let value = tracker.getMinCardValue(); value <= ACE_VALUE; value++) {
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
    // If any attack has been covered, passing is impossible - defender is committed
    if (game.table_battles.some(b => b.defense !== null)) {
        return { canDefinitelyPassBack: false, definitelyCannotPassBack: true, seatsBetween: 0 };
    }
    
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
    
    // The constraint for pass impossibility is a + d > 4 (attacks + distance > 4 copies)
    // This is already checked in isPassDefinitelyImpossible, so we don't duplicate it here.
    // NOTE: Removed incorrect check "totalAttacks > playersInPassChain" - a single passer
    // with one card CAN pass any number of attacks by adding their card to the pile.
    
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
        const isAccounted = tracker.isCardAccountedFor(key);
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

// Use CardTracker.isCardAccountedFor() instead of this duplicate function
const ACE_VALUE = 13;

function calculatePassProbability(
    game: Game,
    attackerId: string,
    attackValue: number,
    numCards: number,
    tracker: CardTracker
): number {
    // If any attack has been covered, passing is impossible - defender is committed to defending
    if (game.table_battles.some(b => b.defense !== null)) {
        return 0;
    }
    
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
    // IMPORTANT: If we KNOW some passers have the rank, we only need to calculate
    // the probability for passers who DON'T have known cards of that rank.
    
    // Filter to only passers who don't have known cards of this rank
    const unknownPasserIds: string[] = [];
    const unknownPasserHandSizes: number[] = [];
    for (let i = 0; i < passerIds.length; i++) {
        if (!preCheck.knownPasserHasRank[i]) {
            unknownPasserIds.push(passerIds[i]);
            const player = game.players.find(p => p.player_id === passerIds[i]);
            unknownPasserHandSizes.push(player?.hand.length ?? 6);
        }
    }
    
    // If all passers are known to have the rank, probability is 1
    const effectiveDistance = unknownPasserIds.length;
    if (effectiveDistance === 0) {
        return 1;
    }
    
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
    
    // R = remaining copies available for the UNKNOWN passers
    // copiesInPasserHands counts known cards in passer hands - those are "used" by known passers
    // Only copiesUnknown are available for the unknown passers
    const effectiveR = preCheck.copiesUnknown;
    
    const DEBUG_PASS_PROB = false; // Temporarily enable debug
    if (DEBUG_PASS_PROB) {
        console.log(`[DEBUG calculatePassProbability] value=${attackValue} (${VALUE_NAMES[attackValue]})`);
        console.log(`  original distance=${distance}, effectiveDistance=${effectiveDistance}, attackSize=${attackSize}`);
        console.log(`  knownPasserHasRank=[${preCheck.knownPasserHasRank.join(', ')}]`);
        console.log(`  preCheck: copiesUnknown=${preCheck.copiesUnknown}, copiesInPasserHands=${preCheck.copiesInPasserHands}, copiesAccountedElsewhere=${preCheck.copiesAccountedElsewhere}`);
        console.log(`  effectiveR=${effectiveR}, totalUnknown=${totalUnknown}`);
        console.log(`  unknownPasserHandSizes=[${unknownPasserHandSizes.join(', ')}]`);
    }
    
    if (effectiveR <= 0) {
        if (DEBUG_PASS_PROB) console.log(`  -> returning 0 (effectiveR <= 0)`);
        return 0;
    }
    
    // Check if there are enough copies for unknown passers
    if (effectiveR < effectiveDistance) {
        if (DEBUG_PASS_PROB) console.log(`  -> returning 0 (effectiveR < effectiveDistance)`);
        return 0;
    }
    
    // Use the inclusion-exclusion formula with effectiveR override
    // Only calculate for passers who don't have known cards
    const prob = calcPassProbFormula(effectiveDistance, attackSize, totalUnknown, unknownPasserHandSizes, effectiveR);
    if (DEBUG_PASS_PROB) console.log(`  -> prob=${prob}`);
    return prob;
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
    const defender = game.players.find(p => p.player_id === playerId);
    
    // Values currently on table BEFORE this cover
    const existingTableValues = new Set<number>();
    for (const battle of game.table_battles) {
        existingTableValues.add(battle.attack.value);
        if (battle.defense) existingTableValues.add(battle.defense.value);
    }
    
    // NEW values introduced by this cover (not already on table)
    const newValues = new Set<number>();
    for (const card of coverCards) {
        if (!existingTableValues.has(card.value)) {
            newValues.add(card.value);
        }
    }
    
    // Calculate P(AllowsAtk) - probability that cover unlocks at least one new throw-in
    const { prob: probAllowsAdditionalAttack, debugInfo: attackDebug } = calculateProbCoverAllowsAttack(
        game, playerId, coverCards, newValues, tracker
    );
    
    // Calculate P(DrawBetter) - probability we draw better cards by value
    const { prob: probDrawBetterCard, avgUnknownValue, cardValue } = calculateProbDrawBetter(
        game, playerId, coverCards, tracker
    );
    
    const stats: CoverStats = {
        probAllowsAdditionalAttack,
        probDrawBetterCard
    };
    
    const debug: CoverDebugInfo = {
        unknownTotal: game.deck.length + tracker.getUnknownCardCount(),
        coverValue: coverCards[0]?.value || 0,
        valueCardsRemaining: attackDebug.newValueCardsUnknown,
        attackerUnknownHand: attackDebug.totalAttackerHands,
        canCoverValues: '',
        avgUnknownValue,
        cardValue
    };
    
    return { stats, debug };
}

/**
 * Calculate probability that covering unlocks at least one new throw-in for attackers.
 * 
 * P(AllowsAtk) = 0 if:
 *   - Cover introduces no new values (Δ = ∅)
 *   - Defender will be out of cards after covering (no more attacks possible)
 *   - Max attacks already reached
 * 
 * P(AllowsAtk) = 1 if:
 *   - Any attacker is KNOWN to have a card matching a newly introduced value
 * 
 * Otherwise, uses hypergeometric formula:
 *   P = 1 - C(N-M, H) / C(N, H)
 * where:
 *   N = total unknown cards
 *   M = number of unknown cards with newly introduced values  
 *   H = total hand size of eligible attackers
 */
function calculateProbCoverAllowsAttack(
    game: Game,
    defenderId: string,
    coverCards: Card[],
    newValues: Set<number>,
    tracker: CardTracker
): { prob: number; debugInfo: { newValueCardsUnknown: number; totalAttackerHands: number } } {
    const defender = game.players.find(p => p.player_id === defenderId);
    
    // If no new values introduced, probability is 0
    if (newValues.size === 0) {
        return { prob: 0, debugInfo: { newValueCardsUnknown: 0, totalAttackerHands: 0 } };
    }
    
    // If defender will have no cards left after covering, no more attacks possible
    const remainingDefenderCards = defender ? defender.hand.length - coverCards.length : 0;
    if (remainingDefenderCards <= 0) {
        return { prob: 0, debugInfo: { newValueCardsUnknown: 0, totalAttackerHands: 0 } };
    }
    
    // Attackers can throw in more cards if defender has cards remaining
    // NOTE: We don't check current table attacks because those are being covered.
    // The question is: CAN attackers throw in more? Yes, if defender has cards remaining.
    
    // Check if any attacker is KNOWN to have a card of a new value
    let totalAttackerHands = 0;
    for (let i = 0; i < game.players.length; i++) {
        if (i === game.defender) continue;
        const player = game.players[i];
        if (player.status !== PLAYER_STATUS.IN) continue;
        
        totalAttackerHands += player.hand.length;
        
        const knownCards = tracker.knownCardsByPlayer.get(player.player_id) || new Set<string>();
        for (const key of knownCards) {
            const [, value] = key.split('-').map(Number);
            if (newValues.has(value)) {
                // Definitely can attack with this value
                return { prob: 1.0, debugInfo: { newValueCardsUnknown: 4, totalAttackerHands } };
            }
        }
    }
    
    // Count M = unknown cards with newly introduced values
    let M = 0;
    for (const value of newValues) {
        for (let suit = 0; suit < 4; suit++) {
            if (!tracker.isCardAccountedFor(`${suit}-${value}`)) {
                M++;
            }
        }
    }
    
    if (M === 0) {
        // All cards of new values are accounted for (in hands we know, discard, etc.)
        return { prob: 0, debugInfo: { newValueCardsUnknown: 0, totalAttackerHands } };
    }
    
    // N = total unknown cards (deck + unknown cards in opponent hands)
    const N = game.deck.length + tracker.getUnknownCardCount();
    
    // H = total cards in attacker hands (subtract known cards since we already checked those)
    let H = 0;
    for (let i = 0; i < game.players.length; i++) {
        if (i === game.defender) continue;
        const player = game.players[i];
        if (player.status !== PLAYER_STATUS.IN) continue;
        
        const knownCards = tracker.knownCardsByPlayer.get(player.player_id) || new Set<string>();
        H += Math.max(0, player.hand.length - knownCards.size);
    }
    
    if (H === 0 || N === 0) {
        return { prob: 0, debugInfo: { newValueCardsUnknown: M, totalAttackerHands } };
    }
    
    // P = 1 - C(N-M, H) / C(N, H)
    // = probability at least one of H cards is among the M cards with new values
    const prob = 1 - hypergeometricProbZero(N, M, H);
    
    return { prob: Math.max(0, Math.min(1, prob)), debugInfo: { newValueCardsUnknown: M, totalAttackerHands } };
}

/**
 * Calculate probability that discarding these cover cards and drawing will result in better cards.
 * "Better" means higher value (treating trump as +100 value).
 * 
 * Uses actual count of cards better than each cover card in the deck.
 */
function calculateProbDrawBetter(
    game: Game,
    defenderId: string,
    coverCards: Card[],
    tracker: CardTracker
): { prob: number; avgUnknownValue: number; cardValue: number } {
    // No deck = no drawing
    if (game.deck.length === 0 && !game.flipped) {
        return { prob: 0, avgUnknownValue: 0, cardValue: coverCards[0]?.value || 0 };
    }
    
    if (coverCards.length === 0) {
        return { prob: 0, avgUnknownValue: 0, cardValue: 0 };
    }
    
    const avgUnknownValue = tracker.getAverageUnknownCardValue();
    const powerSuit = game.power_suit;
    
    // For each cover card, calculate probability of drawing a better card
    let totalProbSum = 0;
    let firstCardValue = 0;
    
    for (const coverCard of coverCards) {
        const coverValue = getCardValue(coverCard, powerSuit);
        if (!firstCardValue) firstCardValue = coverValue;
        
        // Count how many unknown cards in deck are better than this cover card
        let betterCardsInDeck = 0;
        let totalCardsInDeck = game.deck.length + (game.flipped ? 1 : 0);
        
        // We need to count unknown cards that are "better" by value
        // Better = higher value, or trump beats non-trump
        for (let suit = 0; suit < 4; suit++) {
            for (let value = tracker.getMinCardValue(); value <= ACE_VALUE; value++) {
                const cardKey = `${suit}-${value}`;
                // Only count cards in the deck (not accounted for = could be in deck or opponent hands)
                // We approximate by considering all unaccounted cards and scaling by deck proportion
                if (!tracker.isCardAccountedFor(cardKey)) {
                    const candidateValue = getCardValue({ suit, value }, powerSuit);
                    if (candidateValue > coverValue) {
                        betterCardsInDeck++;
                    }
                }
            }
        }
        
        // Scale by proportion that's in deck vs opponent hands
        const totalUnknown = game.deck.length + tracker.getUnknownCardCount();
        const deckProportion = totalUnknown > 0 ? (game.deck.length + (game.flipped ? 1 : 0)) / totalUnknown : 0;
        const effectiveBetterInDeck = betterCardsInDeck * deckProportion;
        
        // Probability of drawing at least one better card when we draw to refill
        // Simplified: if we draw 1 card, P(better) = betterInDeck / deckSize
        const prob = totalCardsInDeck > 0 ? effectiveBetterInDeck / totalCardsInDeck : 0;
        totalProbSum += Math.max(0, Math.min(1, prob));
    }
    
    return { 
        prob: coverCards.length > 0 ? totalProbSum / coverCards.length : 0,
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
        for (let value = tracker.getMinCardValue(); value <= ACE_VALUE; value++) {
            if (tracker.isCardAccountedFor(`${suit}-${value}`)) {
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
        tracker.getMinCardValue(),  // min card value
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
            for (let value = tracker.getMinCardValue(); value <= ACE_VALUE; value++) {
                if (canCover(virtualAttacks[0], { suit, value }, game.power_suit)) {
                    if (!tracker.isCardAccountedFor(`${suit}-${value}`)) {
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
            for (let value = tracker.getMinCardValue(); value <= ACE_VALUE; value++) {
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
    
    // NOTE: Removed incorrect check "totalAttacks > playersInPassChain"
    // The correct constraint is a + d > 4, checked in isPassDefinitelyImpossible
    
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
