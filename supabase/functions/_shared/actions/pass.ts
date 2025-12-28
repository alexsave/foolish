import { Card, Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, AnimationEvent, ANIMATION_EVENT_TYPE, LOG_TYPE } from '../types.ts';
import { check_win } from '../utils.ts';
import { addLog } from '../log_utils.ts';
import { get_next_player_index, validate_defender_status, verify_cards_in_players_hand, no_cards_left, card_comp, cardDisplay } from '../common_utils.ts';

// Validation function for pass moves
export function validatePass(game: Game, player_id: string, cards: Card[]): void {
    // Can only pass during playing state
    if (game.status !== GAME_STATUS.PLAYING) {
        throw new Error(`Game ${game.id} is not in playing state`);
    }

    if (!cards) {
        throw new Error(`No cards provided`);
    }

    // check if cards all have same value
    if (!cards.every(card => card.value === cards[0].value)) {
        throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} are not all the same value`);
    }

    // check no duplicates
    if (new Set(cards).size !== cards.length) {
        throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // check if player is the defender
    validate_defender_status(game, player_id, true);

    const defender: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;
    verify_cards_in_players_hand(defender, cards);

    // check if there are cards on the table
    if (game.table_battles.length === 0) {
        throw new Error(`No cards on the table`);
    }

    // check passability
    if (game.table_battles.some(battle => battle.defense !== null)) {
        throw new Error(`Cover present, cannot pass`);
    }

    if (!game.table_battles.every(battle => battle.attack.value === cards[0].value)) {
        throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} do not match the values on the table`);
    }

    const next_player_index = get_next_player_index(game, game.defender);
    const next_player = game.players[next_player_index];
    if (next_player.hand.length < cards.length + game.table_battles.length) {
        throw new Error(`Player ${next_player.name} does not have enough cards in their hand to cover ${cards.map(card => cardDisplay(card)).join(', ')}`);
    }
}

// Execution function for pass moves
export async function executePass(game: Game, player_id: string, cards: Card[]): Promise<AnimationEvent[]> {
    const events: AnimationEvent[] = [];
    
    // Guard against modifying game state if game is already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return events;
    }
    
    const defender: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;

    // Add to table and remove from hand
    for (const card of cards) {
        game.table_battles.push({
            attack: card,
            defense: null
        });
    }
    defender.hand = defender.hand.filter(card => !cards.some(mCard => card_comp(card, mCard)));


    // Log the pass event
    addLog(game, {
        game_id: game.id,
        log_type: LOG_TYPE.PASS,
        player_id: player_id,
        card_pairs: cards.map(card => ({ primary: card, target: null })),
        defender_index: null
    });

    // Reset good_timestamp since we now have uncovered attacks
    // (good_players stays - they can still say good once all attacks are covered again)
    game.good_timestamp = null;

    // Capture game state after pass
    const gameStateAfterPass = JSON.parse(JSON.stringify(game));

    // Add animation event for the pass with intermediate game state
    events.push({
        type: ANIMATION_EVENT_TYPE.ATTACK_PASS,
        player_id: player_id,
        cards: cards,
        from_location: 'hand',
        to_location: 'table',
        message: `${defender.name} passed with ${cards.map(c => cardDisplay(c)).join(', ')}`,
        game_state: gameStateAfterPass
    });

    const next_player_index = get_next_player_index(game, game.defender);

    // If the deck is empty, they can get out here
    if (no_cards_left(game) && defender.hand.length === 0) {
        defender.status = PLAYER_STATUS.OUT;
        defender.awaiting_attack = false;
        game.elimination_order.push(defender.player_id);
        
        // Log player going out
        addLog(game, {
            game_id: game.id,
            log_type: LOG_TYPE.PLAYER_OUT,
            player_id: player_id,
            card_pairs: [],
            defender_index: null
        });
        
        // Capture game state after player goes out
        const gameStateAfterOut = JSON.parse(JSON.stringify(game));
        
        // Add animation event for player going out
        events.push({
            type: ANIMATION_EVENT_TYPE.OUT,
            player_id: player_id,
            message: `${defender.name} is out`,
            game_state: gameStateAfterOut
        });
        
        await check_win(game);
        game.defender = next_player_index;
        
        // Log defender change
        addLog(game, {
            game_id: game.id,
            log_type: LOG_TYPE.DEFENDER_CHANGE,
            player_id: null,
            card_pairs: [],
            defender_index: game.defender
        });
    } else {
        game.defender = next_player_index;
        
        // Log defender change
        addLog(game, {
            game_id: game.id,
            log_type: LOG_TYPE.DEFENDER_CHANGE,
            player_id: null,
            card_pairs: [],
            defender_index: game.defender
        });
    }

    const uncovered_cards = game.table_battles.filter(battle => battle.defense === null).length;
    const new_defender: PrivatePlayer = game.players[game.defender];
    const defender_cards = new_defender.hand.length;

    // Check for impossible game state
    if (uncovered_cards > defender_cards) {
        throw new Error('Uncovered cards > defender_cards');
    }
    
    // Game continues in playing state (no status changes needed)
    // The logical state is determined by checking uncovered_cards vs defender_cards
    // - If uncovered_cards === defender_cards: defender can only defend
    // - If uncovered_cards < defender_cards: defender can cover, pickup, or pass
    
    return events;
}

// Combined function with validation
export async function handlePass(game: Game, player_id: string, cards: Card[]): Promise<AnimationEvent[]> {
    validatePass(game, player_id, cards);
    return await executePass(game, player_id, cards);
} 