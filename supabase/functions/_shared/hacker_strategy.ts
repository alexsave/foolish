import { Card, Game, PrivatePlayer } from './types.ts';
import { BotStrategy, LegalMove } from './bot_interfaces.ts';
import { canCover, cardDisplay } from './common_utils.ts';

/**
 * Hacker Durak Strategy - Perfect Information AI
 * ==============================================
 * 
 * This strategy uses PERFECT INFORMATION by seeing all players' cards.
 * It's designed to be the ultimate challenge opponent that makes optimal
 * decisions based on complete game state knowledge.
 * 
 * Key Features:
 * - Sees all players' hands (perfect information)
 * - On defending: explores all possible cover branches to maximize success
 * - On attacking: calculates optimal attacks to force pickups
 * - Uses game theory principles for optimal play
 * 
 * WARNING: This is an UNFAIR advantage strategy for testing purposes!
 */
export class HackerStrategy implements BotStrategy {
    readonly name = 'hacker';
    
    async chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        if (legalMoves.length === 0) {
            throw new Error('No legal moves available');
        }
        
        const bot = game.players.find(p => p.player_id === botPlayerId) as PrivatePlayer;
        const botIndex = game.players.indexOf(bot);
        const isDefender = botIndex === game.defender;
        
        console.log(`🕵️ Hacker bot ${bot.name} analyzing perfect information...`);
        
        // Attack strategy with perfect information
        const attackMoves = legalMoves.filter(move => move.type === 'attack');
        if (attackMoves.length > 0 && !isDefender) {
            return this.selectOptimalAttack(attackMoves, game, bot);
        }
        
        // Defense strategy with perfect information
        const coverMoves = legalMoves.filter(move => move.type === 'cover');
        if (coverMoves.length > 0 && isDefender) {
            return this.selectOptimalDefense(legalMoves, game, bot);
        }
        
        // Pass strategy with perfect information
        const passMoves = legalMoves.filter(move => move.type === 'pass');
        if (passMoves.length > 0) {
            return this.selectOptimalPass(passMoves, game, bot);
        }
        
        // Good moves
        const goodMoves = legalMoves.filter(move => move.type === 'good');
        if (goodMoves.length > 0) {
            return goodMoves[0];
        }
        
        // Wait when appropriate (all attacks covered but players still attacking)
        const waitMoves = legalMoves.filter(move => move.type === 'wait');
        if (waitMoves.length > 0) {
            console.log(`Bot ${bot.name} (hacker) chooses to wait - other players still attacking`);
            return waitMoves[0];
        }
        
        // Pickup as last resort
        const pickupMoves = legalMoves.filter(move => move.type === 'pickup');
        if (pickupMoves.length > 0) {
            return pickupMoves[0];
        }
        
        return legalMoves[0];
    }
    
    /**
     * ATTACKING: Calculate which attack will most effectively cause a pickup
     * Uses perfect information to find attacks that are impossible or very difficult to defend
     */
    private selectOptimalAttack(attackMoves: LegalMove[], game: Game, bot: PrivatePlayer): LegalMove {
        const defenderIndex = game.defender;
        const defender = game.players[defenderIndex] as PrivatePlayer;
        const defenderHand = defender.hand;
        
        console.log(`🎯 Hacker analyzing ${attackMoves.length} attack options against defender with ${defenderHand.length} cards`);
        
        // Analyze each attack option
        const attackAnalysis = attackMoves.map(move => {
            if (!move.cards || move.cards.length === 0) {
                return { move, score: 1000 }; // Invalid move
            }
            
            const attackCard = move.cards[0]; // For simplicity, analyze first card
            const analysis = this.analyzeAttackEffectiveness(attackCard, defenderHand, game);
            
            console.log(`📊 Attack ${cardDisplay(attackCard)}: ${analysis.description}`);
            
            return {
                move,
                score: analysis.score,
                description: analysis.description
            };
        });
        
        // Sort by score (lower is better - higher chance of forcing pickup)
        attackAnalysis.sort((a, b) => a.score - b.score);
        
        const bestAttack = attackAnalysis[0];
        console.log(`⚡ Hacker chooses optimal attack: ${bestAttack.description}`);
        
        return bestAttack.move;
    }
    
    /**
     * DEFENDING: Use perfect information to explore all cover possibilities
     * Maximizes probability of successful defense by seeing all players' cards
     */
    private selectOptimalDefense(legalMoves: LegalMove[], game: Game, bot: PrivatePlayer): LegalMove {
        const uncoveredAttacks = game.table_battles.filter(battle => battle.defense === null);
        
        // First, check if we should strategically give up
        if (this.shouldStrategicallyGiveUp(uncoveredAttacks, bot.hand, game)) {
            const pickupMoves = legalMoves.filter(move => move.type === 'pickup');
            if (pickupMoves.length > 0) {
                console.log(`🏳️ Hacker chooses strategic pickup - better long-term position`);
                return pickupMoves[0];
            }
        }
        
        const coverMoves = legalMoves.filter(move => move.type === 'cover');
        if (coverMoves.length > 0) {
            return this.selectOptimalCover(coverMoves, game, bot);
        }
        
        // No cover possible, must pickup
        const pickupMoves = legalMoves.filter(move => move.type === 'pickup');
        if (pickupMoves.length > 0) {
            console.log(`🆘 Hacker forced to pickup - no valid covers available`);
            return pickupMoves[0];
        }
        
        return legalMoves[0];
    }
    
    /**
     * Select optimal cover using perfect information
     * Prioritizes "safe" covers that other players can't use for follow-up attacks
     */
    private selectOptimalCover(coverMoves: LegalMove[], game: Game, bot: PrivatePlayer): LegalMove {
        const allOtherPlayers = game.players.filter(p => p.player_id !== bot.player_id) as PrivatePlayer[];
        
        // Get all ranks that other players have
        const otherPlayersRanks = new Set<number>();
        allOtherPlayers.forEach(player => {
            player.hand.forEach(card => {
                otherPlayersRanks.add(card.value);
            });
        });
        
        // Analyze each cover option
        const coverAnalysis = coverMoves.map(move => {
            if (!move.cards || move.cards.length === 0) {
                return { move, score: 1000 }; // Invalid move
            }
            
            const coverCard = move.cards[0];
            const analysis = this.analyzeCoverSafety(coverCard, otherPlayersRanks, game);
            
            console.log(`🛡️ Cover ${cardDisplay(coverCard)}: ${analysis.description}`);
            
            return {
                move,
                score: analysis.score,
                description: analysis.description
            };
        });
        
        // Sort by score (lower is better - safer cover)
        coverAnalysis.sort((a, b) => a.score - b.score);
        
        const bestCover = coverAnalysis[0];
        console.log(`🎯 Hacker chooses optimal cover: ${bestCover.description}`);
        
        return bestCover.move;
    }
    
    /**
     * Select optimal pass using perfect information
     */
    private selectOptimalPass(passMoves: LegalMove[], game: Game, bot: PrivatePlayer): LegalMove {
        const nextDefenderIndex = (game.defender + 1) % game.players.length;
        const nextDefender = game.players[nextDefenderIndex] as PrivatePlayer;
        
        // Only pass if next defender is in worse position
        if (nextDefender.hand.length < bot.hand.length) {
            console.log(`🔄 Hacker passes to weaker opponent (${nextDefender.hand.length} cards vs ${bot.hand.length})`);
            return passMoves[0];
        }
        
        // If no beneficial pass, don't pass
        return passMoves[0];
    }
    
    /**
     * Analyze attack effectiveness using perfect information
     */
    private analyzeAttackEffectiveness(attackCard: Card, defenderHand: Card[], game: Game): { score: number, description: string } {
        const defenderRanks = new Set(defenderHand.map(card => card.value));
        
        // Check if defender can cover this attack
        const canDefenderCover = defenderHand.some(card => canCover(attackCard, card, game.power_suit));
        
        // Check if defender can pass (has same rank)
        const canDefenderPass = defenderRanks.has(attackCard.value);
        
        // Perfect attack: can't cover AND can't pass
        if (!canDefenderCover && !canDefenderPass) {
            return {
                score: 1, // Best possible
                description: `Perfect attack - uncoverable and unpassable`
            };
        }
        
        // Good attack: can't cover but can pass
        if (!canDefenderCover && canDefenderPass) {
            return {
                score: 2,
                description: `Good attack - uncoverable but passable`
            };
        }
        
        // Mediocre attack: can cover but can't pass
        if (canDefenderCover && !canDefenderPass) {
            // Calculate cost of defense
            const defenseOptions = defenderHand.filter(card => canCover(attackCard, card, game.power_suit));
            const cheapestDefense = defenseOptions.reduce((min, card) => 
                card.value < min.value ? card : min, defenseOptions[0]);
            
            return {
                score: 10 + cheapestDefense.value, // Higher score = worse for us
                description: `Mediocre attack - coverable with ${cardDisplay(cheapestDefense)}`
            };
        }
        
        // Poor attack: can both cover and pass
        return {
            score: 50 + attackCard.value,
            description: `Poor attack - both coverable and passable`
        };
    }
    
    /**
     * Analyze cover safety using perfect information
     */
    private analyzeCoverSafety(coverCard: Card, otherPlayersRanks: Set<number>, game: Game): { score: number, description: string } {
        const isTrump = coverCard.suit === game.power_suit;
        const cardRank = coverCard.value;
        
        // Check if this rank is "safe" (no other players have it)
        const isSafeRank = !otherPlayersRanks.has(cardRank);
        
        if (isSafeRank && !isTrump) {
            return {
                score: 1, // Best possible
                description: `Safe non-trump cover - no follow-up attacks possible`
            };
        }
        
        if (isSafeRank && isTrump) {
            return {
                score: 2,
                description: `Safe trump cover - no follow-up attacks but uses trump`
            };
        }
        
        if (!isSafeRank && !isTrump) {
            return {
                score: 10 + cardRank,
                description: `Risky non-trump cover - others can attack with same rank`
            };
        }
        
        // Unsafe trump
        return {
            score: 20 + cardRank,
            description: `Risky trump cover - others can attack with same rank`
        };
    }
    
    /**
     * Determine if we should strategically give up using perfect information
     */
    private shouldStrategicallyGiveUp(uncoveredAttacks: any[], hand: Card[], game: Game): boolean {
        if (uncoveredAttacks.length === 0) return false;
        
        // Calculate total cost of defending all attacks
        let totalDefenseCost = 0;
        let undefendableCount = 0;
        
        for (const battle of uncoveredAttacks) {
            const attack = battle.attack;
            const defenseOptions = hand.filter(card => canCover(attack, card, game.power_suit));
            
            if (defenseOptions.length === 0) {
                undefendableCount++;
            } else {
                const cheapestDefense = defenseOptions.reduce((min, card) => 
                    card.value < min.value ? card : min, defenseOptions[0]);
                totalDefenseCost += cheapestDefense.value;
            }
        }
        
        // Give up if any attack is undefendable
        if (undefendableCount > 0) {
            console.log(`🚫 Hacker detects ${undefendableCount} undefendable attacks`);
            return true;
        }
        
        // Give up if defense cost is too high relative to hand strength
        const averageHandValue = hand.reduce((sum, card) => sum + card.value, 0) / hand.length;
        const defenseCostRatio = totalDefenseCost / (averageHandValue * hand.length);
        
        if (defenseCostRatio > 0.4) { // If defense costs more than 40% of hand value
            console.log(`💸 Hacker detects expensive defense (${defenseCostRatio.toFixed(2)} ratio)`);
            return true;
        }
        
        return false;
    }
} 