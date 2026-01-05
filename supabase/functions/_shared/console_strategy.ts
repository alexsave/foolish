import { Game } from './types.ts';
import { BotStrategy, LegalMove } from './bot_interfaces.ts';
import { cardDisplay } from './common_utils.ts';
import { SUIT_MAP } from './constants.ts';

/**
 * Console strategy - prompts the user for input to choose moves
 * Allows a human to play against bots via the terminal
 */

export class ConsoleStrategy implements BotStrategy {
    readonly name = 'console';
    
    async chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        if (legalMoves.length === 0) {
            throw new Error('No legal moves available');
        }
        
        // Find player
        const player = game.players.find(p => p.player_id === botPlayerId);
        const playerName = player ? player.name : 'Unknown Player';
        
        // Display game state
        console.log('\n' + '='.repeat(80));
        console.log(`🎮 YOUR TURN: ${playerName}`);
        console.log('='.repeat(80));
        
        // Show player's hand
        console.log('\n📋 Your hand:');
        if (player && player.hand) {
            const handDisplay = player.hand.map(c => cardDisplay(c)).join(', ');
            console.log(`   ${handDisplay}`);
        }
        
        // Show table battles
        if (game.table_battles.length > 0) {
            console.log('\n🃏 Table battles:');
            game.table_battles.forEach((battle, idx) => {
                const attack = cardDisplay(battle.attack);
                const defense = battle.defense 
                    ? cardDisplay(battle.defense)
                    : '❌ (uncovered)';
                console.log(`   ${idx + 1}. ${attack} vs ${defense}`);
            });
        }
        
        // Show other players
        console.log('\n👥 Other players:');
        game.players.forEach((p, idx) => {
            if (p.player_id !== botPlayerId) {
                const isDefender = idx === game.defender;
                const marker = isDefender ? '🛡️ ' : '⚔️ ';
                console.log(`   ${marker}${p.name}: ${p.hand_length} cards`);
            }
        });
        
        // Show power suit
        console.log(`\n🎯 Trump suit: ${SUIT_MAP[game.power_suit]}`);
        console.log(`📚 Deck: ${game.deck_length} cards remaining`);
        
        // Display legal moves
        console.log('\n✨ Available moves:');
        legalMoves.forEach((move, index) => {
            let description = `   ${index + 1}. ${move.type.toUpperCase()}`;
            if (move.cards) {
                description += ` [${move.cards.map(c => cardDisplay(c)).join(', ')}]`;
            }
            if (move.attack_cards) {
                description += ` covering [${move.attack_cards.map(c => cardDisplay(c)).join(', ')}]`;
            }
            console.log(description);
        });
        
        // Prompt for input using Deno's stdin
        console.log('\n' + '='.repeat(80));
        const choice = await this.promptUser(`Enter move number (1-${legalMoves.length}): `);
        
        const choiceNum = parseInt(choice);
        if (isNaN(choiceNum) || choiceNum < 1 || choiceNum > legalMoves.length) {
            console.log('❌ Invalid choice, selecting first move by default');
            return legalMoves[0];
        }
        
        const chosenMove = legalMoves[choiceNum - 1];
        console.log(`✅ You chose: ${chosenMove.type}`);
        console.log('='.repeat(80) + '\n');
        
        return chosenMove;
    }
    
    private async promptUser(question: string): Promise<string> {
        // Use Node.js readline for interactive input
        const readline = await import('readline');
        
        const rl = readline.createInterface({
            input: process.stdin,
            output: process.stdout
        });
        
        return new Promise((resolve) => {
            rl.question(question, (answer) => {
                rl.close();
                resolve(answer.trim());
            });
        });
    }
}

