import { Card, Game, PrivatePlayer } from './types.ts';
import { BotStrategy, LegalMove } from './bot_interfaces.ts';
import { canCover, card_comp } from './common_utils.ts';

// Handwritten strategy - never done attacking, always attack with as many cards as possible
// Replicates the logic from aiDefend() and chooseAttack() functions
export class HandwrittenBotStrategy implements BotStrategy {
    readonly name = 'handwritten';
    
    // Calculate card score: value + (1000 if power suit)
    private cardScore(card: Card, powerSuit: number): number {
        return card.value + (card.suit === powerSuit ? 1000 : 0);
    }
    
    // Choose the lowest value card, preferring non-power suit over power suit
    private chooseLowestValueCards(cards: Card[], powerSuit: number): Card[] {
        if (cards.length === 0) return [];
        
        // Sort cards by preference: non-power suit cards first, then by value ascending
        const sortedCards = [...cards].sort((a, b) => {
            // If one is power suit and other is not, prefer non-power suit
            if (a.suit === powerSuit && b.suit !== powerSuit) return 1;
            if (a.suit !== powerSuit && b.suit === powerSuit) return -1;
            
            // Both same suit type, prefer lower value
            return a.value - b.value;
        });
        
        // Return all cards of the same "preference level" (same suit type and value)
        const lowestCard = sortedCards[0];
        return sortedCards.filter(card => 
            (card.suit === powerSuit) === (lowestCard.suit === powerSuit) &&
            card.value === lowestCard.value
        );
    }
    
    // Find the best covering combination using recursive logic from aiDefend
    private findBestCoverCombination(
        game: Game, 
        botPlayer: PrivatePlayer, 
        coverMoves: LegalMove[]
    ): LegalMove | null {
        if (coverMoves.length === 0) return null;
        
        let bestMove: LegalMove | null = null;
        let bestScore = Infinity;
        
        // Calculate score for each cover move
        coverMoves.forEach(move => {
            if (move.type === 'cover' && move.cards) {
                // Calculate total score by multiplying individual card scores
                let totalScore = 1;
                move.cards.forEach(card => {
                    totalScore *= this.cardScore(card, game.power_suit);
                });
                
                if (totalScore < bestScore) {
                    bestScore = totalScore;
                    bestMove = move;
                }
            }
        });
        
        return bestMove;
    }
    
    async chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        if (legalMoves.length === 0) {
            throw new Error('No legal moves available');
        }
        
        // Find bot for debug logging
        const bot = game.players.find(p => p.player_id === botPlayerId);
        const botName = bot ? bot.name : 'Unknown Bot';
        
        // Filter attack moves to only those with done_attacking_this_round = false
        const continueAttackMoves = legalMoves.filter(move => 
            move.type === 'attack' && move.done_attacking_this_round === false
        );
        
        // If we have continue attack moves, choose the one with most cards but lowest values
        if (continueAttackMoves.length > 0) {
            // Sort by number of cards descending, pick the one with most cards
            continueAttackMoves.sort((a, b) => (b.cards?.length || 0) - (a.cards?.length || 0));
            const maxCards = continueAttackMoves[0].cards?.length || 0;
            
            // Filter to all moves with maximum cards
            const maxCardMoves = continueAttackMoves.filter(move => 
                (move.cards?.length || 0) === maxCards
            );
            
            // Among moves with max cards, choose the one with lowest value cards
            let bestAttackMove = maxCardMoves[0];
            let bestAttackScore = Infinity;
            
            maxCardMoves.forEach(move => {
                if (move.cards) {
                    // Calculate total score for this attack combination
                    let totalScore = 0;
                    move.cards.forEach(card => {
                        totalScore += this.cardScore(card, game.power_suit);
                    });
                    
                    if (totalScore < bestAttackScore) {
                        bestAttackScore = totalScore;
                        bestAttackMove = move;
                    }
                }
            });
            
            console.log(`Bot ${botName} (handwritten) chose attack with ${bestAttackMove.cards?.length || 0} lowest value cards`);
            return bestAttackMove;
        }
        
        // Handle pass moves (similar to attack logic - prefer lowest value)
        const passMoves = legalMoves.filter(move => move.type === 'pass');
        if (passMoves.length > 0) {
            // Choose pass move with lowest value cards
            let bestPass = passMoves[0];
            let bestPassScore = Infinity;
            
            passMoves.forEach(move => {
                if (move.cards) {
                    let totalScore = 0;
                    move.cards.forEach(card => {
                        totalScore += this.cardScore(card, game.power_suit);
                    });
                    
                    if (totalScore < bestPassScore) {
                        bestPassScore = totalScore;
                        bestPass = move;
                    }
                }
            });
            
            console.log(`Bot ${botName} (handwritten) chose pass with lowest value cards`);
            return bestPass;
        }
        
        // Handle cover moves with smart defense logic (after pass preference)
        const coverMoves = legalMoves.filter(move => move.type === 'cover');
        if (coverMoves.length > 0) {
            // Count uncovered attacks - only cover if we can cover ALL of them
            const uncoveredAttacks = game.table_battles.filter(battle => battle.defense === null);
            
            // Find cover moves that cover all uncovered attacks
            const fullCoverMoves = coverMoves.filter(move => 
                move.attack_cards && move.attack_cards.length === uncoveredAttacks.length
            );
            
            if (fullCoverMoves.length > 0) {
                const bestCover = this.findBestCoverCombination(game, bot as PrivatePlayer, fullCoverMoves);
                if (bestCover) {
                    console.log(`Bot ${botName} (handwritten) chose to cover all ${uncoveredAttacks.length} attacks`);
                    return bestCover;
                }
            } else {
                console.log(`Bot ${botName} (handwritten) cannot cover all ${uncoveredAttacks.length} attacks - will not cover partially`);
            }
        }
        
        // Prefer wait over other moves when available
        const waitMoves = legalMoves.filter(move => move.type === 'wait');
        if (waitMoves.length > 0) {
            console.log(`Bot ${botName} (handwritten) chose to wait - other players still attacking`);
            return waitMoves[0];
        }

        // For other non-attack moves (good), choose randomly - but NOT pickup or wait
        const nonAttackNonPickupMoves = legalMoves.filter(move => 
            move.type !== 'attack' && move.type !== 'cover' && move.type !== 'pass' && move.type !== 'pickup' && move.type !== 'wait'
        );
        if (nonAttackNonPickupMoves.length > 0) {
            const randomIndex = Math.floor(Math.random() * nonAttackNonPickupMoves.length);
            const chosenMove = nonAttackNonPickupMoves[randomIndex];
            console.log(`Bot ${botName} (handwritten) chose non-attack: ${chosenMove.type}`);
            return chosenMove;
        }
        
        // If only "done attacking" moves available, still choose them but prefer lowest value cards
        const doneAttackMoves = legalMoves.filter(move => move.type === 'attack');
        if (doneAttackMoves.length > 0) {
            // Sort by number of cards descending, then by card value ascending
            doneAttackMoves.sort((a, b) => {
                const cardDiff = (b.cards?.length || 0) - (a.cards?.length || 0);
                if (cardDiff !== 0) return cardDiff;
                
                // Same number of cards, prefer lower value
                const aScore = (a.cards || []).reduce((sum, card) => sum + this.cardScore(card, game.power_suit), 0);
                const bScore = (b.cards || []).reduce((sum, card) => sum + this.cardScore(card, game.power_suit), 0);
                return aScore - bScore;
            });
            
            const chosenMove = doneAttackMoves[0];
            console.log(`Bot ${botName} (handwritten) forced to choose done attack with ${chosenMove.cards?.length || 0} lowest value cards`);
            return chosenMove;
        }
        
        // Pickup as absolute last resort
        const pickupMoves = legalMoves.filter(move => move.type === 'pickup');
        if (pickupMoves.length > 0) {
            console.log(`Bot ${botName} (handwritten) forced to pickup - no other options`);
            return pickupMoves[0];
        }
        
        // Fallback to random move (should never reach here)
        const randomIndex = Math.floor(Math.random() * legalMoves.length);
        const chosenMove = legalMoves[randomIndex];
        console.log(`Bot ${botName} (handwritten) fallback to random: ${chosenMove.type}`);
        return chosenMove;
    }
} 