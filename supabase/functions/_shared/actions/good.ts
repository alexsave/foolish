import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS } from '../types.ts';
import { refillPlayerHands } from '../common_utils.ts';
import { get_next_player_index } from '../common_utils.ts';
import { check_win } from '../utils.ts';

// Validation function for good moves
export function validateGood(game: Game, player_id: string): void {
    if (game.status !== GAME_STATUS.WAIT_FOR_ATTACKERS) {
        throw new Error(`Game ${game.id} is not in wait_for_attackers mode`);
    }

    const player = game.players.find(player => player.player_id === player_id)!;
    if (player.status !== PLAYER_STATUS.IN) {
        throw new Error(`Player ${player_id} is not ready to attack`);
    }
}

// Execution function for good moves
export async function executeGood(game: Game, player_id: string): Promise<void> {
    // Guard against modifying game state if game is already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return;
    }
    
    const player: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;

    // set them to done attacking
    player.awaiting_attack = false;

    // check if all players are done attacking
    const playable_players = game.players.filter(player => 
        player.player_id !== game.players[game.defender].player_id && 
        player.hand.some(card => game.table_battles.some(battle => battle.attack.value === card.value || (battle.defense && battle.defense.value === card.value))) &&
        player.awaiting_attack &&
        !player.done_attacking_this_round);

    if (playable_players.length !== 0) {
        return;
    }

    // we are done attacking, shift positions
    // Count cards being discarded before clearing table_battles
    const discardedCards = game.table_battles.length * 2; // Each battle has attack + defense
    game.discard_pile_length += discardedCards;
    
    game.table_battles = [];
    refillPlayerHands(game);
    
    game.first_attacker = game.defender;
    game.defender = get_next_player_index(game, game.first_attacker);
    
    // Reset done_attacking_this_round flag for all players when attacking shifts
    game.players.forEach(player => {
        player.done_attacking_this_round = false;
    });
    
    // Check if game should end after refilling - at the very end
    await check_win(game);
    
    // @ts-ignore - check_win() above can change status to GAME_OVER
    if (game.status !== GAME_STATUS.GAME_OVER) {
        game.status = GAME_STATUS.FIRST_ATTACKER;
    }
}

// Combined function with validation
export async function handleGood(game: Game, player_id: string): Promise<void> {
    validateGood(game, player_id);
    await executeGood(game, player_id);
} 