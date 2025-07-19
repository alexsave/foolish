import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, AnimationEvent, ANIMATION_EVENT_TYPE } from '../types.ts';
import { refillPlayerHandsWithEvents } from '../common_utils.ts';
import { get_next_player_index } from '../common_utils.ts';
import { check_win } from '../utils.ts';

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

    // Must be awaiting attack to say good
    if (!player.awaiting_attack) {
        throw new Error(`Player is not awaiting attack`);
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

    // set them to done attacking
    player.awaiting_attack = false;

    // check if all players are done attacking
    const playable_players = game.players.filter(player => 
        player.player_id !== game.players[game.defender].player_id && 
        player.hand.some(card => game.table_battles.some(battle => battle.attack.value === card.value || (battle.defense && battle.defense.value === card.value))) &&
        player.awaiting_attack &&
        !player.done_attacking_this_round);

    if (playable_players.length !== 0) {
        return events;
    }

    // Add animation event for magic transition
    events.push({
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `${player.name} said good - proceeding to next round`
    });

    // we are done attacking, shift positions
    // Count cards being discarded before clearing table_battles
    const discardedCards = game.table_battles.length * 2; // Each battle has attack + defense
    game.discard_pile_length += discardedCards;
    
    // Add animation event for cards going to discard pile
    const allTableCards = game.table_battles.flatMap(battle => 
        battle.defense ? [battle.attack, battle.defense] : [battle.attack]
    );
    console.log('[GOOD ACTION] Table battles before clearing:', game.table_battles);
    console.log('[GOOD ACTION] All table cards for discard:', allTableCards);
    if (allTableCards.length > 0) {
        const discardEvent: AnimationEvent = {
            type: ANIMATION_EVENT_TYPE.CARDS_TO_TRASH,
            cards: allTableCards,
            from_location: 'table',
            to_location: 'discard',
            message: `${allTableCards.length} cards discarded`
        };
        console.log('[GOOD ACTION] Adding cards_to_trash event:', discardEvent);
        events.push(discardEvent);
    } else {
        console.log('[GOOD ACTION] No cards to discard - table is empty');
    }
    
    game.table_battles = [];
    
    // Refill player hands and get animation events
    const { refillEvents } = refillPlayerHandsWithEvents(game);
    events.push(...refillEvents);
    
    game.first_attacker = game.defender;
    game.defender = get_next_player_index(game, game.first_attacker);
    
    // Reset done_attacking_this_round flag for all players when attacking shifts
    game.players.forEach(player => {
        player.done_attacking_this_round = false;
    });
    
    // Check if game should end after refilling - at the very end
    await check_win(game);
    
    // Game continues in playing state (no status change needed unless game is over)
    console.log('[GOOD ACTION] Final events array:', events);
    return events;
}

// Combined function with validation
export async function handleGood(game: Game, player_id: string): Promise<AnimationEvent[]> {
    validateGood(game, player_id);
    return await executeGood(game, player_id);
} 