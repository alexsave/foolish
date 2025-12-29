import { Game } from './types.ts';
import { BotStrategy, LegalMove } from './bot_interfaces.ts';

/**
 * Random bot strategy - picks a random legal move
 * Can be seeded for reproducible testing
 */

let seed = Date.now();

export function setRandomSeed(newSeed: number) {
    seed = newSeed;
}

// Seeded random number generator (LCG algorithm)
function seededRandom(): number {
    seed = (seed * 1664525 + 1013904223) % 4294967296;
    return seed / 4294967296;
}

// Random strategy implementation
export class RandomBotStrategy implements BotStrategy {
    readonly name = 'random';
    
    async chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        if (legalMoves.length === 0) {
            throw new Error('No legal moves available');
        }
        
        // Find bot for debug logging
        const bot = game.players.find(p => p.player_id === botPlayerId);
        const botName = bot ? bot.name : 'Unknown Bot';
        
        // Create a clean summary of legal moves
        const moveSummary = legalMoves.map((move, index) => {
            let description = `${index + 1}. ${move.type}`;
            if (move.cards) {
                description += ` [${move.cards.map(c => `${c.value}${['♠','♥','♦','♣'][c.suit]}`).join(', ')}]`;
            }
            if (move.attack_cards) {
                description += ` covering [${move.attack_cards.map(c => `${c.value}${['♠','♥','♦','♣'][c.suit]}`).join(', ')}]`;
            }
            if (move.type === 'attack' && move.done_attacking_this_round !== undefined) {
                description += move.done_attacking_this_round ? ' (done)' : ' (continue)';
            }
            return description;
        }).join(', ');
        
        console.log(`Bot ${botName} has ${legalMoves.length} moves: ${moveSummary}`);
        
        // Choose a random legal move using seeded random
        const randomIndex = Math.floor(seededRandom() * legalMoves.length);
        const chosenMove = legalMoves[randomIndex];
        
        // Log the chosen move concisely
        let chosenDescription = `${chosenMove.type}`;
        if (chosenMove.cards) {
            chosenDescription += ` [${chosenMove.cards.map(c => `${c.value}${['♠','♥','♦','♣'][c.suit]}`).join(', ')}]`;
        }
        if (chosenMove.attack_cards) {
            chosenDescription += ` covering [${chosenMove.attack_cards.map(c => `${c.value}${['♠','♥','♦','♣'][c.suit]}`).join(', ')}]`;
        }
        if (chosenMove.type === 'attack' && chosenMove.done_attacking_this_round !== undefined) {
            chosenDescription += chosenMove.done_attacking_this_round ? ' (done)' : ' (continue)';
        }
        console.log(`Bot ${botName} chose: ${chosenDescription}`);
        
        return chosenMove;
    }
} 