import { Game } from '../types.ts';
import { BotStrategy, LegalMove } from '../bot_interfaces.ts';
import { cardDisplay, getCardValue } from '../common_utils.ts';
import { SUIT_MAP } from '../constants.ts';
import { CardTracker } from '../durakai/cardTracker.ts';
import { calculateMoveStats, formatMoveStats } from './move_stats.ts';

/**
 * Console strategy - prompts the user for input to choose moves
 * Allows a human to play against bots via the terminal
 */
// TODO: color the trump card options, and sort the options in a rough good to bad order

export class ConsoleStrategy implements BotStrategy {
    readonly name = 'console';
    
    async chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        if (legalMoves.length === 0) {
            throw new Error('No legal moves available');
        }
        
        // Sort moves from best to worst
        const sortedMoves = this.sortMoves(legalMoves, game.power_suit);
        
        // Create card tracker for stats
        const tracker = new CardTracker(game, botPlayerId);
        
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
                console.log(`   ${marker}${p.name}: ${p.hand.length} cards`);
            }
        });
        
        // Show power suit and flipped card
        console.log(`\n🎯 Trump suit: ${SUIT_MAP[game.power_suit]}`);
        const flippedDisplay = game.flipped ? ` (flipped: ${cardDisplay(game.flipped)})` : '';
        console.log(`📚 Deck: ${game.deck.length} cards remaining${flippedDisplay}`);
        
        // Display card tracking information
        this.displayCardTracking(game, botPlayerId, tracker);
        
        // Display legal moves (sorted from best to worst) with stats
        console.log('\n✨ Available moves:');
        sortedMoves.forEach((move, index) => {
            let description = `   ${index + 1}. ${move.type.toUpperCase()}`;
            if (move.cards) {
                description += ` [${move.cards.map(c => cardDisplay(c)).join(', ')}]`;
            }
            if (move.attack_cards) {
                description += ` covering [${move.attack_cards.map(c => cardDisplay(c)).join(', ')}]`;
            }
            console.log(description);
            
            // Calculate and display stats for this move
            const { stats, debug } = calculateMoveStats(game, botPlayerId, move, tracker);
            const statsStr = formatMoveStats(stats, debug ?? undefined);
            if (statsStr) {
                console.log(statsStr);
            }
        });
        
        // Prompt for input using Deno's stdin
        console.log('\n' + '='.repeat(80));
        const choice = await this.promptUser(`Enter move number (1-${sortedMoves.length}): `);
        
        const choiceNum = parseInt(choice);
        if (isNaN(choiceNum) || choiceNum < 1 || choiceNum > sortedMoves.length) {
            console.log('❌ Invalid choice, selecting first move by default');
            return sortedMoves[0];
        }
        
        const chosenMove = sortedMoves[choiceNum - 1];
        console.log(`✅ You chose: ${chosenMove.type}`);
        console.log('='.repeat(80) + '\n');
        
        return chosenMove;
    }
    
    private displayCardTracking(game: Game, botPlayerId: string, tracker: CardTracker): void {
        console.log('\n📊 Card Tracking:');
        
        // Table cards
        if (game.table_battles.length > 0) {
            const tableCards: string[] = [];
            for (const battle of game.table_battles) {
                tableCards.push(cardDisplay(battle.attack));
                if (battle.defense) {
                    tableCards.push(cardDisplay(battle.defense));
                }
            }
            console.log(`   🎴 Table (${tableCards.length}): ${tableCards.join(', ')}`);
        } else {
            console.log(`   🎴 Table: empty`);
        }
        
        // Discard pile
        const discardedCards = tracker.getCardsInDiscard();
        if (discardedCards.length > 0) {
            console.log(`   🗑️  Discard (${discardedCards.length}): ${discardedCards.map(c => cardDisplay(c)).join(', ')}`);
        } else {
            console.log(`   🗑️  Discard: empty`);
        }
        
        // Known cards per player and unknown count
        console.log('\n   👁️  Known opponent cards:');
        for (const p of game.players) {
            if (p.player_id === botPlayerId) continue;
            
            const knownCards = tracker.knownCardsByPlayer.get(p.player_id);
            const knownCount = knownCards?.size || 0;
            const unknownCount = p.hand.length - knownCount;
            
            if (knownCount > 0) {
                const knownCardsList: string[] = [];
                for (const cardKey of knownCards!) {
                    const [suit, value] = cardKey.split('-').map(Number);
                    knownCardsList.push(cardDisplay({ suit, value }));
                }
                console.log(`      ${p.name}: ${knownCardsList.join(', ')} (${unknownCount} unknown)`);
            } else {
                console.log(`      ${p.name}: none known (${unknownCount} unknown)`);
            }
        }
        
        // Calculate unknown cards (cards not in my hand, discard, flipped, table, or known to opponents)
        const unknownCards: { suit: number; value: number }[] = [];
        const me = game.players.find(p => p.player_id === botPlayerId);
        const myHandKeys = new Set(me?.hand.map(c => `${c.suit}-${c.value}`) || []);
        const discardKeys = new Set(discardedCards.map(c => `${c.suit}-${c.value}`));
        const flippedKey = game.flipped ? `${game.flipped.suit}-${game.flipped.value}` : null;
        
        // Cards currently on the table
        const tableKeys = new Set<string>();
        for (const battle of game.table_battles) {
            tableKeys.add(`${battle.attack.suit}-${battle.attack.value}`);
            if (battle.defense) {
                tableKeys.add(`${battle.defense.suit}-${battle.defense.value}`);
            }
        }
        
        const allKnownOpponentKeys = new Set<string>();
        for (const knownCards of tracker.knownCardsByPlayer.values()) {
            for (const key of knownCards) {
                allKnownOpponentKeys.add(key);
            }
        }
        
        // Determine start value based on total cards in game
        // 36 cards = values 5-13 (6-A), 52 cards = values 1-13 (2-A)
        const totalCards = game.discard_pile_length + game.deck.length + 
            (game.flipped ? 1 : 0) + 
            game.players.reduce((sum, p) => sum + p.hand.length, 0);
        const startValue = totalCards <= 36 ? 5 : 1;
        const ACE_VALUE = 13;
        
        for (let suit = 0; suit < 4; suit++) {
            for (let value = startValue; value <= ACE_VALUE; value++) {
                const key = `${suit}-${value}`;
                if (!myHandKeys.has(key) && !discardKeys.has(key) && key !== flippedKey && 
                    !tableKeys.has(key) && !allKnownOpponentKeys.has(key)) {
                    unknownCards.push({ suit, value });
                }
            }
        }
        
        // Group unknown cards by suit for display
        console.log(`\n   ❓ Unknown cards (${unknownCards.length}):`);
        const bySuit: Map<number, { suit: number; value: number }[]> = new Map();
        for (const card of unknownCards) {
            if (!bySuit.has(card.suit)) bySuit.set(card.suit, []);
            bySuit.get(card.suit)!.push(card);
        }
        for (const [suit, cards] of bySuit) {
            const suitName = SUIT_MAP[suit];
            console.log(`      ${suitName}: ${cards.map(c => cardDisplay(c)).join(', ')}`);
        }
    }
    
    private sortMoves(moves: LegalMove[], powerSuit: number): LegalMove[] {
        // Sort moves from best to worst based on the rules:
        // 1. Pass > Cover > Attack > Good > Wait > Pickup
        // 2. More cards is better
        // 3. Within same card count, lower values are better
        
        const moveTypePriority: Record<string, number> = {
            'pass': 1,
            'cover': 2,
            'attack': 3,
            'good': 4,
            'wait': 5,
            'pickup': 6
        };
        
        return [...moves].sort((a, b) => {
            // First, sort by move type priority
            const aPriority = moveTypePriority[a.type] || 99;
            const bPriority = moveTypePriority[b.type] || 99;
            
            if (aPriority !== bPriority) {
                return aPriority - bPriority;
            }
            
            // Same move type - prefer more cards
            const aCardCount = a.cards?.length || 0;
            const bCardCount = b.cards?.length || 0;
            
            if (aCardCount !== bCardCount) {
                return bCardCount - aCardCount; // Descending (more is better)
            }
            
            // Same card count - prefer lower card values (using getCardValue which adds trump penalty)
            if (a.cards && b.cards && a.cards.length > 0 && b.cards.length > 0) {
                const aTotalValue = a.cards.reduce((sum, card) => sum + getCardValue(card, powerSuit), 0);
                const bTotalValue = b.cards.reduce((sum, card) => sum + getCardValue(card, powerSuit), 0);
                
                return aTotalValue - bTotalValue; // Ascending (lower is better)
            }
            
            return 0;
        });
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

