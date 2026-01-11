import { Card, Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, AnimationEvent, ANIMATION_EVENT_TYPE, LOG_TYPE } from '../types.ts';
import { addLog, cloneGame } from '../common_utils.ts';
import { validate_defender_status, verify_cards_in_players_hand, cardDisplay, card_comp } from '../common_utils.ts';

// Validation function for attack moves
export function validateAttack(game: Game, player_id: string, cards: Card[]): void {
    if (!cards || cards.length === 0) {
        throw new Error('No cards provided');
    }

    // Can only attack during playing state
    if (game.status !== GAME_STATUS.PLAYING) {
        throw new Error(`Game ${game.id} is not in playing state`);
    }

    const attacker: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;
    if (!attacker) {
        throw new Error(`Player ${player_id} not found in game`);
    }

    // Player cannot be the defender
    validate_defender_status(game, player_id, false);

    // verify cards in hand
    verify_cards_in_players_hand(attacker, cards);

    // check no duplicates
    if (new Set(cards).size !== cards.length) {
        throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // Determine if this is a first attack or subsequent attack
    const isFirstAttack = game.table_battles.length === 0;
    
    if (isFirstAttack) {
        // First attacker must play cards of same value
        if (!cards.every(card => card.value === cards[0].value)) {
            throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} are not all the same value`);
        }
        
        // Check if player is first attacker
        if (game.players[game.first_attacker].player_id !== player_id) {
            throw new Error(`Player ${player_id} is not the first attacker`);
        }
    } else {
        // Subsequent attacks: every value has to be on the table
        if (!cards.every(card => game.table_battles.some(battle => battle.attack.value === card.value || battle.defense?.value === card.value))) {
            throw new Error(`Some card values of ${cards.map(card => cardDisplay(card)).join(', ')} are not on the table`);
        }
    }
    
    // CRITICAL: Check if defender has enough cards to cover all uncovered attacks
    // Count current uncovered attacks
    const uncoveredAttacks = game.table_battles.filter(battle => battle.defense === null).length;
    const defender = game.players[game.defender];
    
    // After this attack, total uncovered attacks
    const totalUncoveredAfterAttack = uncoveredAttacks + cards.length;
    
    // Defender must have enough cards in hand to potentially cover all uncovered attacks
    if (defender.hand.length < totalUncoveredAfterAttack) {
        throw new Error(`Defender ${defender.name} only has ${defender.hand.length} card(s) but would need to cover ${totalUncoveredAfterAttack} attacks`);
    }
}

// Execution function for attack moves
export function executeAttack(game: Game, player_id: string, cards: Card[]): AnimationEvent[] {
    const events: AnimationEvent[] = [];
    
    // Guard against modifying game state if game is already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return events;
    }
    
    const attacker: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;

    // remove the cards from the hand
    attacker.hand = attacker.hand.filter(card => !cards.some(attack_card => card_comp(card, attack_card)));

    // add the cards to the table
    for (const card of cards) {
        game.table_battles.push({
            attack: card,
            defense: null
        });
    }

    // Log the attack event
    addLog(game, {
        game_id: game.id,
        log_type: LOG_TYPE.ATTACK,
        player_id: player_id,
        card_pairs: cards.map(card => ({ primary: card, target: null })),
        defender_index: null
    });

    // Reset good fields since game state has changed (new attacks added)
    // Players who said "good" to the previous state may want to reconsider
    game.good_timestamp = null;
    game.good_players = [];

    // Capture game state after attack
    const gameStateAfterAttack = cloneGame(game);

    // Add animation event for the attack with intermediate game state
    events.push({
        type: ANIMATION_EVENT_TYPE.ATTACK_PASS,
        player_id: player_id,
        cards: cards,
        from_location: 'hand',
        to_location: 'table',
        message: `${attacker.name} attacked with ${cards.map(c => cardDisplay(c)).join(', ')}`,
        game_state: gameStateAfterAttack
    });

    // Check if attacker has no cards left
    if (attacker.hand.length === 0) {
        // Attacker wins this round
        attacker.status = PLAYER_STATUS.OUT;
        attacker.awaiting_attack = false;
        game.elimination_order.push(attacker.player_id);
        
        // Log player going out
        addLog(game, {
            game_id: game.id,
            log_type: LOG_TYPE.PLAYER_OUT,
            player_id: player_id,
            card_pairs: [],
            defender_index: null
        });
        
        // Capture game state after player goes out
        const gameStateAfterOut = cloneGame(game);
        
        // Add animation event for player going out
        events.push({
            type: ANIMATION_EVENT_TYPE.OUT,
            player_id: player_id,
            message: `${attacker.name} is out`,
            game_state: gameStateAfterOut
        });
        
        return events;
    }

    // Update awaiting_attack flags based on game state
    const isFirstAttack = game.table_battles.length === cards.length;
    
    if (isFirstAttack) {
        // After first attack, set awaiting_attack for all players except defender
        for (let i = 0; i < game.players.length; i++) {
            if (i !== game.defender) {
                game.players[i].awaiting_attack = true;
            }
        }
    } else {
        // Subsequent attacks: attacker continues to await
        attacker.awaiting_attack = true;
    }

    return events;
}

// Combined function with validation
export function handleAttack(game: Game, player_id: string, cards: Card[]): AnimationEvent[] {
    validateAttack(game, player_id, cards);
    return executeAttack(game, player_id, cards);
} 