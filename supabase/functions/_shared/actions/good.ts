import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, AnimationEvent, ANIMATION_EVENT_TYPE } from '../types.ts';
import { refillPlayerHandsWithEvents } from '../common_utils.ts';
import { get_next_player_index } from '../common_utils.ts';
import { check_win } from '../utils.ts';
import { lockedAutoDiscardLoop } from '../auto_discard_loop.ts';

// Shared logic for discarding cards and transitioning to next round
// Used by both player "good" actions and auto-discard timeout
export async function executeRoundTransition(game: Game, reason: string): Promise<AnimationEvent[]> {
    const events: AnimationEvent[] = [];
    
    // Guard against modifying game state if game is already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return events;
    }

    console.log(`${reason} - proceeding to next round`);

    // Add animation event for magic transition
    const gameStateForTransition = JSON.parse(JSON.stringify(game));
    events.push({
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `${reason} - proceeding to next round`,
        game_state: gameStateForTransition
    });

    // Count cards being discarded before clearing table_battles
    const discardedCards = game.table_battles.length * 2; // Each battle has attack + defense
    game.discard_pile_length += discardedCards;
    
    // Add animation event for cards going to discard pile
    const allTableCards = game.table_battles.flatMap(battle => 
        battle.defense ? [battle.attack, battle.defense] : [battle.attack]
    );
    
    // Clear table battles
    game.table_battles = [];
    
    if (allTableCards.length > 0) {
        const gameStateAfterDiscard = JSON.parse(JSON.stringify(game));
        const discardEvent: AnimationEvent = {
            type: ANIMATION_EVENT_TYPE.CARDS_TO_TRASH,
            cards: allTableCards,
            from_location: 'table',
            to_location: 'discard',
            message: `${allTableCards.length} cards discarded`,
            game_state: gameStateAfterDiscard
        };
        events.push(discardEvent);
    }
    
    // Refill player hands and capture states for each refill event
    const { refillEvents } = refillPlayerHandsWithEvents(game);
    for (const refillEvent of refillEvents) {
        // The refillPlayerHandsWithEvents already modified the game state
        const gameStateAfterRefill = JSON.parse(JSON.stringify(game));
        events.push({
            ...refillEvent,
            game_state: gameStateAfterRefill
        });
    }
    
    game.first_attacker = game.defender;
    game.defender = get_next_player_index(game, game.first_attacker);
    
    // Reset done_attacking_this_round flag for all players when attacking shifts
    game.players.forEach(player => {
        player.done_attacking_this_round = false;
    });
    
    // Reset good fields when round ends
    game.good_players = [];
    game.good_timestamp = null;
    
    // Check if game should end after refilling - at the very end
    await check_win(game);
    
    // Game continues in playing state (no status change needed unless game is over)
    return events;
}

// Validation function for good moves
export function validateGood(game: Game, player_id: string): void {
    // Can only say good during playing state
    if (game.status !== GAME_STATUS.PLAYING) {
        throw new Error(`Game ${game.id} is not in playing state`);
    }

    const player = game.players.find(player => player.player_id === player_id)!;
    if (player.status !== PLAYER_STATUS.IN) {
        throw new Error(`Player ${player_id} is not ready to attack`);
    }

    // Can only say good when all attacks are covered and player is not defender
    if (game.players[game.defender].player_id === player_id) {
        throw new Error(`Defender cannot say good`);
    }

    // Can only say good when all attacks are covered
    if (!game.table_battles.every(battle => battle.defense !== null)) {
        throw new Error(`Cannot say good - not all attacks are covered`);
    }

    // Player must not have already said good
    if (game.good_players && game.good_players.includes(player_id)) {
        throw new Error(`Player has already said good`);
    }
}

// Execution function for good moves
export async function executeGood(game: Game, player_id: string): Promise<AnimationEvent[]> {
    const events: AnimationEvent[] = [];
    
    // Guard against modifying game state if game is already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return events;
    }
    
    const player: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;

    // Initialize good_players array if it doesn't exist
    if (!game.good_players) {
        game.good_players = [];
    }

    // Add this player to the list of players who have said good
    if (!game.good_players.includes(player_id)) {
        game.good_players.push(player_id);
    }

    // Set them to done attacking
    player.awaiting_attack = false;

    // Get all attackers (non-defender, non-out players)
    const allAttackers = game.players.filter((p, index) => 
        index !== game.defender && 
        p.status === PLAYER_STATUS.IN
    );

    // Check if all attackers have pressed good
    const allAttackersGood = allAttackers.every(attacker => 
        game.good_players.includes(attacker.player_id)
    );

    // Check if 1 minute has passed since good_timestamp was set
    const oneMinutePassed = game.good_timestamp !== null && game.good_timestamp !== undefined
        ? (Date.now() - game.good_timestamp >= 60000) 
        : false;

    // Only proceed to next round if all attackers have pressed good OR 1 minute has passed
    if (!allAttackersGood && !oneMinutePassed) {
        console.log(`Good pressed by ${player.name}. Waiting for other attackers or timeout. ` +
            `${game.good_players.length}/${allAttackers.length} attackers ready. ` +
            `Time remaining: ${game.good_timestamp ? Math.max(0, 60000 - (Date.now() - game.good_timestamp)) : 60000}ms`);
        
        // Trigger auto-discard loop to monitor timeout (fire-and-forget)
        // Only call if we're not advancing yet - performance optimization
        lockedAutoDiscardLoop(game.id).catch(error => {
            console.error(`Error starting auto-discard loop for game ${game.id}:`, error);
        });
        
        return events;
    }

    // All attackers said good OR timeout reached - proceed to next round
    const transitionReason = allAttackersGood 
        ? `All ${allAttackers.length} attackers said good`
        : `1 minute timeout reached (${game.good_players.length}/${allAttackers.length} attackers ready)`;

    // Use shared round transition logic
    return await executeRoundTransition(game, transitionReason);
}

// Combined function with validation
export async function handleGood(game: Game, player_id: string): Promise<AnimationEvent[]> {
    validateGood(game, player_id);
    return await executeGood(game, player_id);
} 