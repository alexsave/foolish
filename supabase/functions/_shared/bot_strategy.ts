import { Card, Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS } from './types.ts';
import { canCover, card_comp, get_next_player_index } from './common_utils.ts';
import { BotStrategy, LegalMove } from './bot_interfaces.ts';
import { RandomBotStrategy } from './random_strategy.ts';
import { OneCardBotStrategy } from './one_card_strategy.ts';
import { HandwrittenBotStrategy } from './handwritten_strategy.ts';
import { SimpleHeuristicStrategy } from './simple_heuristic_strategy.ts';
import { UltimateChampionStrategy } from './ultimate_champion_strategy.ts';
import { ChampionStrategy } from './champion_strategy.ts';
import { HackerStrategy } from './hacker_strategy.ts';

// Re-export interfaces for backwards compatibility
export type { BotStrategy, LegalMove };

// Strategy registry
export const BOT_STRATEGIES: Map<string, BotStrategy> = new Map<string, BotStrategy>([
    ['random', new RandomBotStrategy()],
    ['one_card', new OneCardBotStrategy()],
    ['handwritten', new HandwrittenBotStrategy()],
    ['simple_heuristic', new SimpleHeuristicStrategy()],
    ['ultimate_champion', new UltimateChampionStrategy()],
    ['champion', new ChampionStrategy()],
    ['hacker', new HackerStrategy()],
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
    
    // Game state specific moves based on logical conditions
    if (game.status === GAME_STATUS.PLAYING) {
        const isFirstAttack = game.table_battles.length === 0;
        const allAttacksCovered = game.table_battles.every(battle => battle.defense !== null);
        
        if (isFirstAttack && isFirstAttacker) {
            // Bot is first attacker - can attack with same value cards
            // CANNOT just say "good" - must make a move
            const attackMoves = calculateFirstAttackMoves(game, botPlayer);
            moves.push(...attackMoves);
        } else if (isDefender && game.table_battles.length > 0) {
            // Bot is defender - can cover, pickup, pass, and optionally wait
            const coverMoves = calculateCoverMoves(game, botPlayer);
            moves.push(...coverMoves);
            
            // Can always pickup
            moves.push({ type: 'pickup' });
            
            // Can wait if all attacks are covered and there are players still attacking
            const canWait = allAttacksCovered && hasPlayersStillAttacking(game);
            if (canWait) {
                moves.push({ type: 'wait' });
            }
            
            // Can pass if all attacks are same value and bot has that value
            const passMoves = calculatePassMoves(game, botPlayer);
            moves.push(...passMoves);
        } else if (!isDefender && game.table_battles.length > 0) {
            // Bot is attacker - can attack with cards on table or say "good"
            const attackMoves = calculateRegularAttackMoves(game, botPlayer);
            moves.push(...attackMoves);
            
            // Can only say "good" if awaiting_attack is true and all attacks are covered
            if (botPlayer.awaiting_attack && allAttacksCovered) {
                moves.push({ type: 'good' });
            }
        }
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
            if (!restCombo.coverCards.some(card => card_comp(card, coverCard))) {
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
    
    // Can only pass if ALL attacks are uncovered (no covered battles)
    // This matches the validation in pass.ts: if (game.table_battles.some(battle => battle.defense !== null))
    const hasCoveredBattles = game.table_battles.some(battle => battle.defense !== null);
    
    if (hasCoveredBattles) {
        // Cannot pass if any battle is covered
        return moves;
    }
    
    // All battles must be uncovered at this point
    if (game.table_battles.length === 0) {
        return moves;
    }
    
    // Check if all attacks have same value
    const firstValue = game.table_battles[0].attack.value;
    const allSameValue = game.table_battles.every(battle => battle.attack.value === firstValue);
    
    if (allSameValue) {
        // Find cards with the same value
        const matchingCards = hand.filter(card => card.value === firstValue);
        
        if (matchingCards.length > 0) {
            // Get next player index using the same logic as pass validation
            const nextPlayerIndex = get_next_player_index(game, game.defender);
            const nextPlayer = game.players[nextPlayerIndex];
            
            // Generate all possible combinations of matching cards (1 to all matching cards)
            for (let i = 1; i <= matchingCards.length; i++) {
                const combinations = getCombinations(matchingCards, i);
                
                combinations.forEach(combo => {
                    // Check if next player has enough cards to cover all attacks
                    // This matches pass validation: next_player.hand.length < cards.length + game.table_battles.length
                    const totalCardsAfterPass = combo.length + game.table_battles.length;
                    
                    if (nextPlayer.hand.length >= totalCardsAfterPass) {
                        // Add both versions: continue attacking and done attacking this round
                        moves.push({
                            type: 'pass',
                            cards: combo,
                            done_attacking_this_round: false
                        });
                        moves.push({
                            type: 'pass',
                            cards: combo,
                            done_attacking_this_round: true
                        });
                    }
                });
            }
        }
    }
    
    return moves;
}

// Helper function to check if there are players still attacking
function hasPlayersStillAttacking(game: Game): boolean {
    // Use the same logic as auto-transition in bot_actions.ts
    const playable_players = game.players.filter(player =>
        player.player_id !== game.players[game.defender].player_id &&
        player.status !== PLAYER_STATUS.OUT &&
        player.hand.some(card => game.table_battles.some(battle => battle.attack.value === card.value || (battle.defense && battle.defense.value === card.value))) &&
        player.awaiting_attack &&
        !player.done_attacking_this_round);
    
    return playable_players.length > 0;
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