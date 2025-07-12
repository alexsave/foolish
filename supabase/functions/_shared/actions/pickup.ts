import { Game, PrivatePlayer, GAME_STATUS } from '../types.ts';
import { get_next_player_index, validate_defender_status, refillPlayerHands } from '../common_utils.ts';

// Validation function for pickup moves
export function validatePickup(game: Game, player_id: string): void {
    if (game.status !== GAME_STATUS.FREE_PLAY && game.status !== GAME_STATUS.ONLY_DEFEND) {
        throw new Error(`Game ${game.id} is not in free_play or only_defend mode`);
    }

    // check if player is the defender
    validate_defender_status(game, player_id, true);

    // check if there are cards on the table
    if (game.table_battles.length === 0) {
        throw new Error(`No cards on the table`);
    }
}

// Execution function for pickup moves
export function executePickup(game: Game, player_id: string): void {
    const defender: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;

    // add cards from table to hand
    game.table_battles.forEach(battle => {
        defender.hand.push(battle.attack);
        if (battle.defense) {
            defender.hand.push(battle.defense);
        }
    });

    // clear table
    game.table_battles = [];

    // Draw cards and shift positions
    refillPlayerHands(game);
    game.first_attacker = get_next_player_index(game, game.defender);
    game.defender = get_next_player_index(game, game.first_attacker);
    game.status = GAME_STATUS.FIRST_ATTACKER;
    
    // Reset done_attacking_this_round flag for all players when attacking shifts
    game.players.forEach(player => {
        player.done_attacking_this_round = false;
    });
}

// Combined function with validation
export function handlePickup(game: Game, player_id: string): void {
    validatePickup(game, player_id);
    executePickup(game, player_id);
} 