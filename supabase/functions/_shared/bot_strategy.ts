import { Card, Game, PrivatePlayer, GAME_STATUS } from './types.ts';
import { canCover, card_comp } from './common_utils.ts';

// Legal moves that a bot can make
export interface LegalMove {
    type: 'attack' | 'cover' | 'pass' | 'pickup' | 'good';
    cards?: Card[];
    attack_cards?: Card[]; // For cover moves, which cards to cover
    done_attacking_this_round?: boolean; // For attack moves, whether to be done attacking this round
}

// Bot strategy interface
export interface BotStrategy {
    // Given the game state and bot's hand, choose a legal move
    chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove>;
    
    // Strategy identifier
    readonly name: string;
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
        
        // Debug logging for random strategy
        console.log(`Bot ${botName} (random) has ${legalMoves.length} legal moves:`);
        for (let index = 0; index < legalMoves.length; index++) {
            const move = legalMoves[index];
            let moveDescription = `  ${index + 1}. ${move.type}`;
            if (move.cards) {
                moveDescription += ` with cards: [${move.cards.map(c => `${c.value}${['♠','♥','♦','♣'][c.suit]}`).join(', ')}]`;
            }
            if (move.attack_cards) {
                moveDescription += ` covering: [${move.attack_cards.map(c => `${c.value}${['♠','♥','♦','♣'][c.suit]}`).join(', ')}]`;
            }
            if (move.type === 'attack' && move.done_attacking_this_round !== undefined) {
                moveDescription += ` (done attacking: ${move.done_attacking_this_round})`;
            }
            console.log(moveDescription);
        }
        // Choose a random legal move
        const randomIndex = Math.floor(Math.random() * legalMoves.length);
        const chosenMove = legalMoves[randomIndex];
        
        // Log the chosen move
        let chosenDescription = `Bot ${botName} chose: ${JSON.stringify(chosenMove)}`;
        if (chosenMove.cards) {
            chosenDescription += ` with cards: [${chosenMove.cards.map(c => `${c.value}${['♠','♥','♦','♣'][c.suit]}`).join(', ')}]`;
        }
        if (chosenMove.attack_cards) {
            chosenDescription += ` covering: [${chosenMove.attack_cards.map(c => `${c.value}${['♠','♥','♦','♣'][c.suit]}`).join(', ')}]`;
        }
        if (chosenMove.type === 'attack' && chosenMove.done_attacking_this_round !== undefined) {
            chosenDescription += ` (done attacking: ${chosenMove.done_attacking_this_round})`;
        }
        console.log(chosenDescription);
        
        return chosenMove;
    }
}

// One card per attack strategy - only puts down one card per attack round
export class OneCardBotStrategy implements BotStrategy {
    readonly name = 'one_card';
    
    async chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        if (legalMoves.length === 0) {
            throw new Error('No legal moves available');
        }
        
        // Filter to only attack moves that use exactly one card AND are done attacking this round
        const singleCardDoneAttackMoves = legalMoves.filter(move => 
            move.type === 'attack' && 
            move.cards && 
            move.cards.length === 1 && 
            move.done_attacking_this_round === true
        );
        
        // If we have single card "done attacking" moves, prefer those
        if (singleCardDoneAttackMoves.length > 0) {
            const randomIndex = Math.floor(Math.random() * singleCardDoneAttackMoves.length);
            return singleCardDoneAttackMoves[randomIndex];
        }
        
        // Otherwise, for non-attack moves, choose randomly
        const nonAttackMoves = legalMoves.filter(move => move.type !== 'attack');
        if (nonAttackMoves.length > 0) {
            const randomIndex = Math.floor(Math.random() * nonAttackMoves.length);
            return nonAttackMoves[randomIndex];
        }
        
        // If only other attack moves available, prefer "done attacking" versions
        const doneAttackMoves = legalMoves.filter(move => 
            move.type === 'attack' && move.done_attacking_this_round === true
        );
        if (doneAttackMoves.length > 0) {
            // Sort by number of cards, pick the one with fewest cards
            doneAttackMoves.sort((a, b) => (a.cards?.length || 0) - (b.cards?.length || 0));
            return doneAttackMoves[0];
        }
        
        // Fallback to random move
        const randomIndex = Math.floor(Math.random() * legalMoves.length);
        return legalMoves[randomIndex];
    }
}

// Strategy registry
export const BOT_STRATEGIES: Map<string, BotStrategy> = new Map<string, BotStrategy>([
    ['random', new RandomBotStrategy()],
    ['one_card', new OneCardBotStrategy()],
]);

// Get strategy by key
export function getBotStrategy(strategyKey: string): BotStrategy {
    const strategy = BOT_STRATEGIES.get(strategyKey);
    if (!strategy) {
        // Fall back to random strategy if unknown
        return BOT_STRATEGIES.get('random')!;
    }
    return strategy;
}

// Calculate all legal moves for a bot given current game state
export function calculateLegalMoves(game: Game, botPlayerId: string): LegalMove[] {
    const moves: LegalMove[] = [];
    
    // Find the bot player
    const botPlayer = game.players.find(p => p.player_id === botPlayerId);
    if (!botPlayer) {
        return moves;
    }
    
    const botIndex = game.players.indexOf(botPlayer);
    const isDefender = botIndex === game.defender;
    const isFirstAttacker = botIndex === game.first_attacker;
    
    // Game state specific moves
    switch (game.status) {
        case GAME_STATUS.FIRST_ATTACKER:
            if (isFirstAttacker) {
                // Bot is first attacker - can attack with same value cards
                // CANNOT just say "good" - must make a move
                const attackMoves = calculateFirstAttackMoves(game, botPlayer);
                moves.push(...attackMoves);
            }
            break;
            
        case GAME_STATUS.FREE_PLAY:
            if (isDefender) {
                // Bot is defender - can cover, pickup, or pass
                const coverMoves = calculateCoverMoves(game, botPlayer);
                moves.push(...coverMoves);
                
                // Can always pickup
                moves.push({ type: 'pickup' });
                
                // Can pass if all attacks are same value and bot has that value
                const passMoves = calculatePassMoves(game, botPlayer);
                moves.push(...passMoves);
            } else {
                // Bot is attacker - can attack with cards on table or say "good"
                const attackMoves = calculateRegularAttackMoves(game, botPlayer);
                moves.push(...attackMoves);
                
                // Can only say "good" if awaiting_attack is true
                if (botPlayer.awaiting_attack) {
                    moves.push({ type: 'good' });
                }
            }
            break;
            
        case GAME_STATUS.ONLY_DEFEND:
            if (isDefender) {
                // Bot is defender - can only cover or pickup
                const coverMoves = calculateCoverMoves(game, botPlayer);
                moves.push(...coverMoves);
                
                // Can always pickup
                moves.push({ type: 'pickup' });
            }
            break;
            
        case GAME_STATUS.WAIT_FOR_ATTACKERS:
            if (!isDefender) {
                // Bot is an attacker - can attack with cards on table or confirm done
                const attackMoves = calculateRegularAttackMoves(game, botPlayer);
                moves.push(...attackMoves);
                
                // Can only say "good" if awaiting_attack is true
                if (botPlayer.awaiting_attack) {
                    moves.push({ type: 'good' });
                }
            }
            break;
    }
    
    return moves;
}

// Calculate first attack moves (must play cards of same value)
function calculateFirstAttackMoves(game: Game, botPlayer: PrivatePlayer): LegalMove[] {
    const moves: LegalMove[] = [];
    const hand = botPlayer.hand;
    
    // Group cards by value
    const valueGroups = new Map<number, Card[]>();
    hand.forEach(card => {
        if (!valueGroups.has(card.value)) {
            valueGroups.set(card.value, []);
        }
        valueGroups.get(card.value)!.push(card);
    });
    
    // For each value, generate all possible combinations (1 to all cards of that value)
    valueGroups.forEach((cards, value) => {
        // Generate all possible combinations (1 to all cards of this value)
        for (let i = 1; i <= cards.length; i++) {
            const combinations = getCombinations(cards, i);
            combinations.forEach(combo => {
                // Check if defender has enough cards to cover
                const defenderCards = game.players[game.defender].hand.length;
                const uncoveredCards = game.table_battles.filter(b => b.defense === null).length;
                
                if (uncoveredCards + combo.length <= defenderCards) {
                    // Add both versions: continue attacking and done attacking this round
                    moves.push({ type: 'attack', cards: combo, done_attacking_this_round: false });
                    moves.push({ type: 'attack', cards: combo, done_attacking_this_round: true });
                }
            });
        }
    });
    
    return moves;
}

// Calculate regular attack moves (must match values on table)
function calculateRegularAttackMoves(game: Game, botPlayer: PrivatePlayer): LegalMove[] {
    const moves: LegalMove[] = [];
    const hand = botPlayer.hand;
    
    // Get all values currently on the table
    const tableValues = new Set<number>();
    game.table_battles.forEach(battle => {
        tableValues.add(battle.attack.value);
        if (battle.defense) {
            tableValues.add(battle.defense.value);
        }
    });
    
    // Find cards that match table values
    const validCards = hand.filter(card => tableValues.has(card.value));
    
    // If no valid cards, return empty array (bot will be skipped)
    if (validCards.length === 0) {
        return moves;
    }
    
    // Generate all possible combinations of valid cards
    for (let i = 1; i <= validCards.length; i++) {
        const combinations = getCombinations(validCards, i);
        combinations.forEach(combo => {
            // Check if defender has enough cards to cover
            const defenderCards = game.players[game.defender].hand.length;
            const uncoveredCards = game.table_battles.filter(b => b.defense === null).length;
            
            if (uncoveredCards + combo.length <= defenderCards) {
                // Add both versions: continue attacking and done attacking this round
                moves.push({ type: 'attack', cards: combo, done_attacking_this_round: false });
                moves.push({ type: 'attack', cards: combo, done_attacking_this_round: true });
            }
        });
    }
    
    return moves;
}

// Calculate cover moves (can cover multiple attacks in various combinations)
function calculateCoverMoves(game: Game, botPlayer: PrivatePlayer): LegalMove[] {
    const moves: LegalMove[] = [];
    const hand = botPlayer.hand;
    
    // Find uncovered attacks
    const uncoveredAttacks = game.table_battles.filter(battle => battle.defense === null);
    
    if (uncoveredAttacks.length === 0) {
        return moves;
    }
    
    // For each uncovered attack, find all cards that can cover it
    const coverOptions: Map<number, Card[]> = new Map();
    uncoveredAttacks.forEach((battle, index) => {
        const attackCard = battle.attack;
        const validCovers = hand.filter(card => canCover(attackCard, card, game.power_suit));
        if (validCovers.length > 0) {
            coverOptions.set(index, validCovers);
        }
    });
    
    // Generate all possible combinations of covering attacks
    // This includes covering 1 attack, 2 attacks, ..., all attacks
    for (let numToCover = 1; numToCover <= uncoveredAttacks.length; numToCover++) {
        const attackIndexCombos = getCombinations(
            Array.from({ length: uncoveredAttacks.length }, (_, i) => i),
            numToCover
        );
        
        attackIndexCombos.forEach(attackIndices => {
            // Check if we can cover all these attacks
            const canCoverAll = attackIndices.every(index => coverOptions.has(index));
            if (!canCoverAll) return;
            
            // Generate all combinations of cards to cover these attacks
            const coverCardCombos = generateCoverCombinations(
                attackIndices.map(index => ({
                    attackIndex: index,
                    attackCard: uncoveredAttacks[index].attack,
                    coverCards: coverOptions.get(index)!
                }))
            );
            
            coverCardCombos.forEach(combo => {
                moves.push({
                    type: 'cover',
                    cards: combo.coverCards,
                    attack_cards: combo.attackCards
                });
            });
        });
    }
    
    return moves;
}

// Helper function to generate all combinations of covering specific attacks
function generateCoverCombinations(
    attackCoverPairs: Array<{ attackIndex: number, attackCard: Card, coverCards: Card[] }>
): Array<{ coverCards: Card[], attackCards: Card[] }> {
    if (attackCoverPairs.length === 0) {
        return [{ coverCards: [], attackCards: [] }];
    }
    
    const [first, ...rest] = attackCoverPairs;
    const restCombinations = generateCoverCombinations(rest);
    const result: Array<{ coverCards: Card[], attackCards: Card[] }> = [];
    
    // For each card that can cover the first attack
    first.coverCards.forEach(coverCard => {
        // For each combination of the rest
        restCombinations.forEach(restCombo => {
            // Make sure we don't use the same card twice
            if (!restCombo.coverCards.some(card => cardEquals(card, coverCard))) {
                result.push({
                    coverCards: [coverCard, ...restCombo.coverCards],
                    attackCards: [first.attackCard, ...restCombo.attackCards]
                });
            }
        });
    });
    
    return result;
}

// Calculate pass moves
function calculatePassMoves(game: Game, botPlayer: PrivatePlayer): LegalMove[] {
    const moves: LegalMove[] = [];
    const hand = botPlayer.hand;
    
    // Can only pass if all attacks are same value and uncovered
    const uncoveredAttacks = game.table_battles.filter(battle => battle.defense === null);
    
    if (uncoveredAttacks.length === 0) {
        return moves;
    }
    
    // Check if all attacks have same value
    const firstValue = uncoveredAttacks[0].attack.value;
    const allSameValue = uncoveredAttacks.every(battle => battle.attack.value === firstValue);
    
    if (allSameValue) {
        // Find cards with the same value
        const matchingCards = hand.filter(card => card.value === firstValue);
        
        // Can pass with any single card of this value
        matchingCards.forEach(card => {
            moves.push({
                type: 'pass',
                cards: [card]
            });
        });
    }
    
    return moves;
}

// Helper function to check if two cards are equal
function cardEquals(card1: Card, card2: Card): boolean {
    return card_comp(card1, card2);
}

// Helper function to generate combinations
function getCombinations<T>(array: T[], size: number): T[][] {
    if (size === 0) return [[]];
    if (size > array.length) return [];
    
    const result: T[][] = [];
    
    for (let i = 0; i <= array.length - size; i++) {
        const first = array[i];
        const rest = getCombinations(array.slice(i + 1), size - 1);
        
        rest.forEach(combo => {
            result.push([first, ...combo]);
        });
    }
    
    return result;
} 