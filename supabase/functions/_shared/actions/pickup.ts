import { Game, PrivatePlayer, GAME_STATUS, AnimationEvent, ANIMATION_EVENT_TYPE } from '../types.ts';
import { get_next_player_index, validate_defender_status, refillPlayerHands } from '../common_utils.ts';
import { check_win } from '../utils.ts';

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
export async function executePickup(game: Game, player_id: string): Promise<AnimationEvent[]> {
    const events: AnimationEvent[] = [];
    
    // Guard against modifying game state if game is already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return events;
    }
    
    const defender: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;

    // Get all cards from table battles for pickup animation
    const allTableCards = game.table_battles.flatMap(battle => 
        battle.defense ? [battle.attack, battle.defense] : [battle.attack]
    );

    // Add animation event for the pickup
    events.push({
        type: ANIMATION_EVENT_TYPE.PICKUP,
        player_id: player_id,
        cards: allTableCards,
        from_location: 'table',
        to_location: 'hand',
        message: `${defender.name} picked up ${allTableCards.length} cards`
    });

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
    
    // Reset done_attacking_this_round flag for all players when attacking shifts
    game.players.forEach(player => {
        player.done_attacking_this_round = false;
    });
    
    // Check if game should end after refilling - at the very end
    await check_win(game);
    // Set game status to first attacker (check_win may have changed it to game_over)
    if (game.status === GAME_STATUS.GAME_OVER) {
        game.status = GAME_STATUS.FIRST_ATTACKER;
    }
    
    return events;
}

// Combined function with validation
export async function handlePickup(game: Game, player_id: string): Promise<AnimationEvent[]> {
    validatePickup(game, player_id);
    return await executePickup(game, player_id);
} 