import { Game, PrivatePlayer, GAME_STATUS, AnimationEvent, ANIMATION_EVENT_TYPE, LOG_TYPE, Card } from '../types.ts';
import { get_next_player_index, validate_defender_status, refillPlayerHandsWithEvents, cloneGame } from '../common_utils.ts';
import { addLog } from '../common_utils.ts';

// Validation function for pickup moves
export function validatePickup(game: Game, player_id: string): void {
    // Can only pickup during playing state
    if (game.status !== GAME_STATUS.PLAYING) {
        throw new Error(`Game ${game.id} is not in playing state`);
    }

    // check if player is the defender
    validate_defender_status(game, player_id, true);

    // check if there are cards on the table
    if (game.table_battles.length === 0) {
        throw new Error(`No cards on the table`);
    }
}

// Execution function for pickup moves
export function executePickup(game: Game, player_id: string): AnimationEvent[] {
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

    // add cards from table to hand
    game.table_battles.forEach(battle => {
        defender.hand.push(battle.attack);
        if (battle.defense) {
            defender.hand.push(battle.defense);
        }
    });

    // clear table
    game.table_battles = [];

    // Log the pickup event (this is critical for bots to know which cards a player has)
    addLog(game, {
        game_id: game.id,
        log_type: LOG_TYPE.PICKUP,
        player_id: player_id,
        card_pairs: allTableCards.map(card => ({ primary: card, target: null })),
        defender_index: null
    });

    // Capture game state after pickup
    const gameStateAfterPickup = cloneGame(game);

    // Add animation event for the pickup with intermediate game state
    events.push({
        type: ANIMATION_EVENT_TYPE.PICKUP,
        player_id: player_id,
        cards: allTableCards,
        from_location: 'table',
        to_location: 'hand',
        message: `${defender.name} picked up ${allTableCards.length} cards`,
        game_state: gameStateAfterPickup
    });

    // Draw cards. Each refill event already carries its own per-iteration
    // snapshot via cloneGame.
    const { refillEvents, drawLogs } = refillPlayerHandsWithEvents(game);
    for (const refillEvent of refillEvents) {
        events.push(refillEvent);
    }
    
    // Add draw logs to game logs
    for (const drawLog of drawLogs) {
        addLog(game, {
            game_id: game.id,
            log_type: LOG_TYPE.DRAW,
            player_id: drawLog.player_id,
            card_pairs: drawLog.cards.map((card: Card) => ({ primary: card, target: null })),
            defender_index: null
        });
    }
    
    game.first_attacker = get_next_player_index(game, game.defender);
    game.defender = get_next_player_index(game, game.first_attacker);
    
    // Log defender change
    addLog(game, {
        game_id: game.id,
        log_type: LOG_TYPE.DEFENDER_CHANGE,
        player_id: null,
        card_pairs: [],
        defender_index: game.defender
    });
    
    // Reset good fields when round ends
    game.good_players = [];
    game.good_timestamp = null;
    
    // Game continues in playing state (no status change needed unless game is over)
    
    return events;
}

// Combined function with validation
export function handlePickup(game: Game, player_id: string): AnimationEvent[] {
    validatePickup(game, player_id);
    return executePickup(game, player_id);
} 