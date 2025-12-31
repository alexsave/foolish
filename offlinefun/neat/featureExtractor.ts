import { Game, Card, PrivatePlayer } from '../types';
import { CardTracker } from './cardTracker';
import { calculateLegalMoves } from '../bot_strategy';
import { LegalMove } from '../bot_interfaces';

/**
 * Extracts numerical features from game state for neural network input
 * Features are normalized to [0, 1] or [-1, 1] range
 * 
 * Total: 36 features (30 calculated + 6 binary move type indicators)
 */
export class FeatureExtractor {
    private tracker: CardTracker;
    private me: PrivatePlayer;
    private includePolynomial: boolean;
    
    constructor(
        private game: Game,
        private myPlayerId: string,
        options?: {
            includePolynomial?: boolean;
        }
    ) {
        this.tracker = new CardTracker(game, myPlayerId);
        this.me = game.players.find(p => p.player_id === myPlayerId)!;
        this.includePolynomial = options?.includePolynomial ?? true;
    }
    
    /**
     * Compute total cards in game (from handwritten strategy)
     */
    private computeTotalCardCount(): number {
        const deckCount = this.game.deck.length;
        const discardCount = this.game.discard_pile_length;
        const tableCount = this.game.table_battles.reduce((sum, battle) => sum + 1 + (battle.defense ? 1 : 0), 0);
        const handsCount = this.game.players.reduce((sum, p) => sum + p.hand.length, 0);
        const flippedCount = this.game.flipped ? 1 : 0;
        return deckCount + discardCount + tableCount + handsCount + flippedCount;
    }

    /**
     * Trump attack probability (from handwritten strategy)
     * Key insight: avoid trumps early, use them late
     */
    private getTrumpAttackProbability(): number {
        // While deck exists, strongly avoid trump attacks
        if (this.game.deck_length > 0 || this.game.flipped !== null) {
            return 0.02;
        }

        // Endgame: scale by discard ratio
        const totalCards = Math.max(1, this.computeTotalCardCount());
        const discardRatio = Math.max(0, Math.min(1, this.game.discard_pile_length / totalCards));
        const p = 0.65 + 0.35 * discardRatio;
        return Math.max(0.5, Math.min(0.95, p));
    }

    /**
     * Card score (from handwritten strategy)
     * Trumps are worth 1000+ their value
     */
    private cardScore(card: Card): number {
        return card.value + (card.suit === this.game.power_suit ? 1000 : 0);
    }
    
    /**
     * Extract features for a specific move
     * Returns normalized feature vector
     */
    extractFeaturesForMove(move: LegalMove): number[] {
        const features: number[] = [];
        
        // === Game phase features (from handwritten strategy) ===
        const totalCards = this.computeTotalCardCount();
        features.push(totalCards / 52); // Total cards in play
        features.push(this.game.deck.length / 52); // Deck size ratio
        features.push(this.game.discard_pile_length / 52); // Discard pile ratio
        features.push(this.game.flipped ? 1 : 0); // Flipped card available
        const discardRatio = totalCards > 0 ? this.game.discard_pile_length / totalCards : 0;
        features.push(discardRatio); // Game progression indicator
        
        // === Trump strategy features (from handwritten) ===
        const trumpAttackProb = this.getTrumpAttackProbability();
        features.push(trumpAttackProb); // When to use trumps
        const handTrumps = this.me.hand.filter(c => c.suit === this.game.power_suit).length;
        features.push(handTrumps / Math.max(1, this.me.hand.length)); // Trump ratio in hand
        features.push(handTrumps / 13); // Absolute trump count
        
        // === Basic game state features ===
        features.push(this.me.hand.length / 12); // My hand size (max ~12)
        features.push(this.game.table_battles.length / 6); // Table size (max 6)

        // Multi-player: encode hand sizes for up to 8 seats.
        // Treat missing seats as "out" (0 cards), so a 2p game looks like 8p with 6 players out.
        const maxSeats = 8;
        const seatHandSizes: number[] = [];
        for (let i = 0; i < maxSeats; i++) {
            const p = this.game.players[i];
            const size = p ? p.hand.length : 0;
            seatHandSizes.push(size / 12);
        }
        // Summary stats across opponents (excluding me)
        const myIndex = this.game.players.findIndex(p => p.player_id === this.myPlayerId);
        const opponentSizes = this.game.players
            .filter(p => p.player_id !== this.myPlayerId)
            .map(p => p.hand.length);
        const oppMax = opponentSizes.length ? Math.max(...opponentSizes) : 0;
        const oppMin = opponentSizes.length ? Math.min(...opponentSizes) : 0;
        const oppAvg = opponentSizes.length ? opponentSizes.reduce((a, b) => a + b, 0) / opponentSizes.length : 0;

        // Add seat features + opponent summaries + normalized player counts
        features.push(...seatHandSizes);        // 8 features
        features.push(oppMax / 12);             // max opponent hand
        features.push(oppMin / 12);             // min opponent hand
        features.push(oppAvg / 12);             // avg opponent hand
        features.push(this.game.players.length / 8); // number of seats in this game
        const playersIn = this.game.players.filter(p => p.hand.length > 0).length;
        features.push(playersIn / 8);           // still-in count (rough)
        features.push((myIndex >= 0 ? myIndex : 0) / 7); // my seat index
        features.push(this.game.defender / 7);  // defender index
        
        // === Card knowledge features ===
        features.push(this.tracker.getKnownOpponentCardCount() / 12); // Known opponent cards
        features.push(this.tracker.getUnknownCardCount() / 52); // Unknown cards
        features.push(this.tracker.getAverageUnknownCardValue() / 14); // Avg unknown value
        
        // === Move-specific features (fixed-width: 6 slots) ===
        // Important for NEAT: we must keep a stable feature layout and length regardless of move type.
        const moveCards = this.getCardsFromMove(move);
        const moveFeatures: number[] = [];

        if (move.type === 'attack') {
            const avgAttackScore =
                moveCards.length > 0 ? moveCards.reduce((sum, c) => sum + this.cardScore(c), 0) / moveCards.length : 0;
            const isTrumpAttack = moveCards.some(c => c.suit === this.game.power_suit) ? 1 : 0;
            let coverProb = 0;
            for (const card of moveCards) coverProb += this.tracker.getProbabilityCanCover(card);
            const avgCoverProb = moveCards.length > 0 ? coverProb / moveCards.length : 0;

            const passProb = moveCards.length > 0 ? this.tracker.getProbabilityCanPass(moveCards[0].value) : 0;

            const beforeHandValue = this.calculateHandValue(this.me.hand);
            const afterHand = this.me.hand.filter(c => !moveCards.some(m => c.suit === m.suit && c.value === m.value));
            const afterHandValue = this.calculateHandValue(afterHand);
            const handValueChange = beforeHandValue > 0 ? (beforeHandValue - afterHandValue) / beforeHandValue : 0;

            moveFeatures.push(
                avgAttackScore / 1014,      // slot 0: avg attack score
                moveCards.length / 4,       // slot 1: num cards
                isTrumpAttack,              // slot 2: uses trump
                avgCoverProb,               // slot 3: defender cover probability
                passProb,                   // slot 4: opponent pass-back probability
                handValueChange             // slot 5: hand value change
            );
        } else if (move.type === 'cover') {
            const avgDefenseValue = moveCards.length > 0 ? moveCards.reduce((sum, c) => sum + c.value, 0) / moveCards.length : 0;
            const usesTrump = moveCards.some(c => c.suit === this.game.power_suit) ? 1 : 0;

            const allCovered =
                this.game.table_battles.every(b => b.defense !== null) &&
                this.game.table_battles.length + moveCards.length >= this.game.table_battles.filter(b => !b.defense).length;

            let additionalAttackProb = 0;
            if (allCovered) {
                const tableValues = new Set(this.game.table_battles.map(b => b.attack.value));
                for (const value of tableValues) {
                    additionalAttackProb = Math.max(additionalAttackProb, this.tracker.getProbabilityOpponentHasValue(value));
                }
            }

            const beforeHandValue = this.calculateHandValue(this.me.hand);
            const afterHand = this.me.hand.filter(c => !moveCards.some(m => c.suit === m.suit && c.value === m.value));
            const afterHandValue = this.calculateHandValue(afterHand);
            const handValueChange = beforeHandValue > 0 ? (beforeHandValue - afterHandValue) / beforeHandValue : 0;

            moveFeatures.push(
                avgDefenseValue / 14,       // slot 0: avg defense value
                moveCards.length / 4,       // slot 1: num cards used
                additionalAttackProb,       // slot 2: prob of more attacks
                handValueChange,            // slot 3: hand value change
                usesTrump,                  // slot 4: used trump
                0                           // slot 5: padding
            );
        } else if (move.type === 'pass') {
            const avgPassValue = moveCards.length > 0 ? moveCards.reduce((sum, c) => sum + c.value, 0) / moveCards.length : 0;
            const totalBurden = this.game.table_battles.length + moveCards.length;
            moveFeatures.push(
                moveCards.length / 4,       // slot 0: num cards passed
                avgPassValue / 14,          // slot 1: avg value
                totalBurden / 6,            // slot 2: burden
                0, 0, 0                     // slots 3-5: padding
            );
        } else if (move.type === 'pickup') {
            const pickupCount = this.game.table_battles.length * 2;
            let avgPickedValue = 0;
            if (pickupCount > 0) {
                let totalValue = 0;
                for (const battle of this.game.table_battles) {
                    totalValue += battle.attack.value;
                    if (battle.defense) totalValue += battle.defense.value;
                }
                avgPickedValue = totalValue / pickupCount;
            }
            moveFeatures.push(
                pickupCount / 12,           // slot 0: pickup size
                avgPickedValue / 14,        // slot 1: avg value
                0, 0, 0, 0                  // slots 2-5: padding
            );
        } else if (move.type === 'good') {
            moveFeatures.push(1, 0, 0, 0, 0, 0);
        } else {
            // wait / unknown
            moveFeatures.push(0, 0, 0, 0, 0, 0);
        }

        // Ensure fixed length 6
        while (moveFeatures.length < 6) moveFeatures.push(0);
        if (moveFeatures.length > 6) moveFeatures.length = 6;
        features.push(...moveFeatures);
        
        // === Strategic features ===
        // Hand strength (weighted by trump and high cards)
        features.push(this.calculateHandStrength(this.me.hand));
        
        // Strongest-opponent strength estimate (based on per-opponent known cards + unknown average)
        features.push(this.estimateStrongestOpponentStrength());
        
        // Game phase (early/mid/late based on deck size)
        const phase = this.game.deck.length > 30 ? 0 : (this.game.deck.length > 10 ? 0.5 : 1);
        features.push(phase);
        
        // Am I defender?
        features.push(this.game.players[this.game.defender].player_id === this.myPlayerId ? 1 : 0);
        
        // === Binary move type indicators (6 features) ===
        features.push(move.type === 'attack' ? 1 : 0);
        features.push(move.type === 'cover' ? 1 : 0);
        features.push(move.type === 'pass' ? 1 : 0);
        features.push(move.type === 'pickup' ? 1 : 0);
        features.push(move.type === 'good' ? 1 : 0);
        features.push(move.type === 'wait' ? 1 : 0);

        // Ensure stable base length (36) even if we add/remove features above.
        // (NEAT expects a fixed-size input vector.)
        // NOTE: base feature count changed for multi-player support; keep stable via getBaseFeatureCount().
        const baseCount = FeatureExtractor.getBaseFeatureCount();
        while (features.length < baseCount) features.push(0);
        if (features.length > baseCount) features.length = baseCount;
        
        // Validate base features
        const hasNaN = features.some((f, i) => {
            if (isNaN(f) || !isFinite(f)) {
                console.warn(`Feature ${i} is NaN/Infinite for move type ${move.type}`);
                return true;
            }
            return false;
        });
        
        // Replace NaN with 0
        if (hasNaN) {
            for (let i = 0; i < features.length; i++) {
                if (isNaN(features[i]) || !isFinite(features[i])) {
                    features[i] = 0;
                }
            }
        }

        // If disabled, return only base features (36)
        if (!this.includePolynomial) {
            return features;
        }

        // === Add polynomial features (all pairwise products) ===
        const baseFeatureCount = features.length;
        const polynomialFeatures: number[] = [];

        for (let i = 0; i < baseFeatureCount; i++) {
            for (let j = 0; j < baseFeatureCount; j++) {
                const product = features[i] * features[j];
                polynomialFeatures.push(isNaN(product) || !isFinite(product) ? 0 : product);
            }
        }

        // Combine base features with polynomial features
        return [...features, ...polynomialFeatures];
    }
    
    private getCardsFromMove(move: LegalMove): Card[] {
        if (move.cards && move.cards.length > 0) {
            return move.cards;
        }
        return [];
    }
    
    private calculateHandValue(hand: Card[]): number {
        if (hand.length === 0) return 0;
        return hand.reduce((sum, c) => sum + c.value, 0);
    }
    
    private calculateHandStrength(hand: Card[]): number {
        if (hand.length === 0) return 0;
        
        let strength = 0;
        for (const card of hand) {
            let cardStrength = card.value / 14; // Base strength
            if (card.suit === this.game.power_suit) {
                cardStrength *= 1.5; // Trump bonus
            }
            strength += cardStrength;
        }
        
        return Math.min(1, strength / hand.length / 1.5); // Normalized
    }
    
    private estimateStrongestOpponentStrength(): number {
        const avgUnknown = this.tracker.getAverageUnknownCardValue();
        let best = 0;

        for (const p of this.game.players) {
            if (p.player_id === this.myPlayerId) continue;
            const handSize = p.hand.length;
            if (handSize <= 0) continue;

            const knownSet = this.tracker.knownCardsByPlayer.get(p.player_id);
            const knownKeys = knownSet ? Array.from(knownSet) : [];
            const knownCount = Math.min(handSize, knownKeys.length);

            let knownSum = 0;
            for (const k of knownKeys) {
                const [, valueStr] = k.split('-');
                const v = Number(valueStr);
                if (Number.isFinite(v)) knownSum += v;
            }
            const knownAvg = knownCount > 0 ? knownSum / knownCount : avgUnknown;
            const unknownCount = Math.max(0, handSize - knownCount);
            const estimatedAvg = (knownAvg * knownCount + avgUnknown * unknownCount) / handSize;
            const strength = Math.min(1, estimatedAvg / 14);

            best = Math.max(best, strength);
        }

        return best;
    }
    
    /**
     * Get feature count for network architecture
     */
    static getFeatureCount(): number {
        const baseFeatures = FeatureExtractor.getBaseFeatureCount();
        const polynomialFeatures = baseFeatures * baseFeatures; // All pairwise products
        return baseFeatures + polynomialFeatures;
    }

    static getBaseFeatureCount(): number {
        // Locked base feature size for NEAT + robustness training.
        // See comment in repo history for the breakdown; do not change without retraining.
        return 44;
    }
}

