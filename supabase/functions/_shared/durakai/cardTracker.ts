import { Game, GameLog, LOG_TYPE, Card } from '../types';

/**
 * Tracks card locations and probabilities based on game logs
 * Provides perfect information about:
 * - Cards definitely in each opponent's hand (from pickups not yet played)
 * - Cards definitely discarded
 * - Probabilistic information about unknown cards
 */
export class CardTracker {
    public knownCardsByPlayer: Map<string, Set<string>>; // Cards we know each player has
    private discardedCards: Set<string>; // Cards in discard pile
    private myCards: Set<string>; // My current hand
    private tableCards: Set<string>; // Cards currently on the table
    private flippedCard: string | null; // The visible trump card under the deck
    private deckSize: number;
    private opponentIds: string[];
    
    constructor(private game: Game, private myPlayerId: string) {
        this.knownCardsByPlayer = new Map();
        this.discardedCards = new Set();
        this.myCards = new Set();
        this.tableCards = new Set();
        this.flippedCard = null;
        this.deckSize = game.deck.length;
        this.opponentIds = game.players
            .filter(p => p.player_id !== myPlayerId)
            .map(p => p.player_id);
        
        // Track the flipped card (visible trump card under the deck)
        if (game.flipped) {
            this.flippedCard = this.cardKey(game.flipped);
        }
        
        // Initialize my cards from my hand
        const myPlayer = game.players.find(p => p.player_id === myPlayerId);
        if (myPlayer) {
            for (const card of myPlayer.hand) {
                this.myCards.add(this.cardKey(card));
            }
        }
        
        // Track cards currently on the table
        for (const battle of game.table_battles) {
            this.tableCards.add(this.cardKey(battle.attack));
            if (battle.defense) {
                this.tableCards.add(this.cardKey(battle.defense));
            }
        }
        
        // Initialize known cards for each opponent
        for (const opponentId of this.opponentIds) {
            this.knownCardsByPlayer.set(opponentId, new Set());
        }
        
        this.analyzeLogs();
    }
    
    private cardKey(card: Card): string {
        return `${card.suit}-${card.value}`;
    }
    
    /** Get the minimum card value based on total cards in game */
    public getMinCardValue(): number {
        // Calculate total cards from public info
        // 36 cards = values 5-13 (6-A), 52 cards = values 1-13 (2-A)
        const totalCards = this.game.discard_pile_length + this.game.deck.length + 
            (this.game.flipped ? 1 : 0) + 
            this.game.players.reduce((sum, p) => sum + p.hand.length, 0);
        return totalCards <= 36 ? 5 : 1;
    }
    
    private readonly ACE_VALUE = 13;
    
    private analyzeLogs(): void {
        // Track cards picked up by each opponent that haven't been played
        const pickedUpByPlayer = new Map<string, Set<string>>();
        for (const opponentId of this.opponentIds) {
            pickedUpByPlayer.set(opponentId, new Set());
        }
        
        for (const log of this.game.logs) {
            switch (log.log_type) {
                case LOG_TYPE.PICKUP:
                    if (log.player_id && log.player_id !== this.myPlayerId) {
                        // Opponent picked up cards - we know they have these
                        const playerCards = pickedUpByPlayer.get(log.player_id);
                        if (playerCards) {
                            for (const pair of log.card_pairs) {
                                if (pair.primary) {
                                    playerCards.add(this.cardKey(pair.primary));
                                }
                            }
                        }
                    }
                    break;
                    
                case LOG_TYPE.ATTACK:
                case LOG_TYPE.COVER:
                case LOG_TYPE.PASS:
                    if (log.player_id && log.player_id !== this.myPlayerId) {
                        // Opponent played these cards - remove from known cards
                        const playerCards = pickedUpByPlayer.get(log.player_id);
                        if (playerCards) {
                            for (const pair of log.card_pairs) {
                                if (pair.primary) {
                                    playerCards.delete(this.cardKey(pair.primary));
                                }
                            }
                        }
                    }
                    break;
                    
                case LOG_TYPE.DISCARD:
                    // Cards were discarded - they're out of the game
                    for (const pair of log.card_pairs) {
                        if (pair.primary) {
                            this.discardedCards.add(this.cardKey(pair.primary));
                        }
                    }
                    break;
            }
        }
        
        this.knownCardsByPlayer = pickedUpByPlayer;
        
        // Track my current hand
        const me = this.game.players.find(p => p.player_id === this.myPlayerId);
        if (me) {
            for (const card of me.hand) {
                this.myCards.add(this.cardKey(card));
            }
        }
    }
    
    /**
     * Get probability that ANY opponent has a specific card value
     * Returns max probability across all opponents
     */
    getProbabilityOpponentHasValue(value: number): number {
        let maxProb = 0;
        
        for (const opponentId of this.opponentIds) {
            const prob = this.getProbabilityPlayerHasValue(opponentId, value);
            maxProb = Math.max(maxProb, prob);
        }
        
        return maxProb;
    }
    
    /**
     * Get probability that a specific opponent has a card value
     */
    getProbabilityPlayerHasValue(playerId: string, value: number): number {
        const knownCards = this.knownCardsByPlayer.get(playerId);
        if (!knownCards) return 0;
        
        // Count how many cards of this value opponent definitely has
        let knownCount = 0;
        for (const cardKey of knownCards) {
            const [, cardValue] = cardKey.split('-').map(Number);
            if (cardValue === value) knownCount++;
        }
        
        if (knownCount > 0) return 1.0; // We know they have at least one
        
        // Calculate probability based on unknown cards
        const unknownCards = this.getUnknownCardCount();
        if (unknownCards === 0) return 0;
        
        // Count how many cards of this value are unaccounted for
        let possibleCards = 0;
        for (let suit = 0; suit < 4; suit++) {
            const key = this.cardKey({ suit, value });
            if (!this.isCardAccountedFor(key)) {
                possibleCards++;
            }
        }
        
        const player = this.game.players.find(p => p.player_id === playerId);
        const playerHandSize = player?.hand.length || 0;
        
        // Probability = 1 - (ways to not have any) / (total ways)
        // Using hypergeometric distribution
        if (possibleCards === 0) return 0;
        if (playerHandSize === 0) return 0;
        
        // Simplified: probability at least one card matches
        return Math.min(1, possibleCards * playerHandSize / unknownCards);
    }
    
    public isCardAccountedFor(cardKey: string): boolean {
        if (this.myCards.has(cardKey)) return true;
        if (this.discardedCards.has(cardKey)) return true;
        if (this.tableCards.has(cardKey)) return true; // Cards on the table
        if (this.flippedCard === cardKey) return true; // Flipped card is visible/known
        
        for (const knownCards of this.knownCardsByPlayer.values()) {
            if (knownCards.has(cardKey)) return true;
        }
        
        return false;
    }
    
    /**
     * Probability ANY opponent (specifically the defender) can cover a specific attack card
     */
    getProbabilityCanCover(attackCard: Card): number {
        const defenderId = this.game.players[this.game.defender]?.player_id;
        if (!defenderId || defenderId === this.myPlayerId) return 0;
        
        return this.getProbabilityPlayerCanCover(defenderId, attackCard);
    }
    
    /**
     * Probability a specific player can cover an attack
     */
    getProbabilityPlayerCanCover(playerId: string, attackCard: Card): number {
        const knownCards = this.knownCardsByPlayer.get(playerId);
        if (!knownCards) return 0;
        
        // Check known cards first
        for (const cardKey of knownCards) {
            const [suit, value] = cardKey.split('-').map(Number);
            const card = { suit, value };
            if (this.canCover(attackCard, card)) {
                return 1.0; // We know they can cover
            }
        }
        
        // Calculate probability from unknown cards
        let canCoverCount = 0;
        const unknownCards = this.getUnknownCardCount();
        if (unknownCards === 0) return 0;
        
        // Count unknown cards that could cover this attack
        for (let suit = 0; suit < 4; suit++) {
            for (let value = this.getMinCardValue(); value <= this.ACE_VALUE; value++) {
                const key = this.cardKey({ suit, value });
                if (!this.isCardAccountedFor(key)) {
                    if (this.canCover(attackCard, { suit, value })) {
                        canCoverCount++;
                    }
                }
            }
        }
        
        const player = this.game.players.find(p => p.player_id === playerId);
        const playerHandSize = player?.hand.length || 0;
        return Math.min(1, canCoverCount * playerHandSize / unknownCards);
    }
    
    private canCover(attack: Card, defense: Card): boolean {
        const trumpSuit = this.game.power_suit;
        
        // Trump always beats non-trump
        if (defense.suit === trumpSuit && attack.suit !== trumpSuit) return true;
        
        // Same suit, higher value
        if (defense.suit === attack.suit && defense.value > attack.value) return true;
        
        return false;
    }
    
    /**
     * Probability opponent can pass (has matching value)
     */
    getProbabilityCanPass(value: number): number {
        return this.getProbabilityOpponentHasValue(value);
    }
    
    getUnknownCardCount(): number {
        // Unknown cards = cards in opponent hands that we don't know about
        // = sum of opponent hand sizes - known cards in opponent hands
        let opponentHandSizes = 0;
        for (const player of this.game.players) {
            if (player.player_id !== this.myPlayerId) {
                opponentHandSizes += player.hand.length;
            }
        }
        
        let knownOpponentCardCount = 0;
        for (const knownCards of this.knownCardsByPlayer.values()) {
            knownOpponentCardCount += knownCards.size;
        }
        
        return opponentHandSizes - knownOpponentCardCount;
    }
    
    getKnownOpponentCardCount(): number {
        let total = 0;
        for (const knownCards of this.knownCardsByPlayer.values()) {
            total += knownCards.size;
        }
        return total;
    }
    
    /**
     * Get known cards for a specific opponent
     */
    getKnownCardsForPlayer(playerId: string): number {
        return this.knownCardsByPlayer.get(playerId)?.size || 0;
    }
    
    getDiscardedCardCount(): number {
        return this.discardedCards.size;
    }
    
    /**
     * Get all cards in discard pile
     */
    getCardsInDiscard(): Card[] {
        const cards: Card[] = [];
        for (const cardKey of this.discardedCards) {
            const [suit, value] = cardKey.split('-').map(Number);
            cards.push({ suit, value });
        }
        return cards;
    }
    
    /**
     * Average value of unknown cards
     */
    getAverageUnknownCardValue(): number {
        let totalValue = 0;
        let count = 0;
        
        for (let suit = 0; suit < 4; suit++) {
            for (let value = this.getMinCardValue(); value <= this.ACE_VALUE; value++) {
                const key = this.cardKey({ suit, value });
                // Check if card is not in my hand, discard, flipped, or any opponent's known cards
                let isKnown = this.myCards.has(key) || this.discardedCards.has(key) || this.flippedCard === key;
                if (!isKnown) {
                    for (const knownCards of this.knownCardsByPlayer.values()) {
                        if (knownCards.has(key)) {
                            isKnown = true;
                            break;
                        }
                    }
                }
                if (!isKnown) {
                    totalValue += value;
                    count++;
                }
            }
        }
        
        return count > 0 ? totalValue / count : 8; // Default to middle value
    }
    
    /**
     * Get the flipped card if visible
     */
    getFlippedCard(): Card | null {
        if (!this.flippedCard) return null;
        const [suit, value] = this.flippedCard.split('-').map(Number);
        return { suit, value };
    }
    
    /**
     * Check if flipped card is still available
     */
    hasFlippedCard(): boolean {
        return this.flippedCard !== null;
    }
}

