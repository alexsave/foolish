import { Card, Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, AnimationEvent, ANIMATION_EVENT_TYPE } from '../types.ts';
import { check_win } from '../utils.ts';
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
}

// Execution function for attack moves
export async function executeAttack(game: Game, player_id: string, cards: Card[]): Promise<AnimationEvent[]> {
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

    // Add animation event for the attack
    events.push({
        type: ANIMATION_EVENT_TYPE.ATTACK_PASS,
        player_id: player_id,
        cards: cards,
        from_location: 'hand',
        to_location: 'table',
        message: `${attacker.name} attacked with ${cards.map(c => cardDisplay(c)).join(', ')}`
    });

    // Check if attacker has no cards left
    if (attacker.hand.length === 0) {
        // Attacker wins this round
        attacker.status = PLAYER_STATUS.OUT;
        attacker.awaiting_attack = false;
        game.elimination_order.push(attacker.player_id);
        
        // Add animation event for player going out
        events.push({
            type: ANIMATION_EVENT_TYPE.OUT,
            player_id: player_id,
            message: `${attacker.name} is out`
        });
        
        await check_win(game);
        return events;
    }

    // Update awaiting_attack flags based on game state
    const isFirstAttack = game.table_battles.length === cards.length; // Was zero before adding these cards
    
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
export async function handleAttack(game: Game, player_id: string, cards: Card[]): Promise<AnimationEvent[]> {
    validateAttack(game, player_id, cards);
    return await executeAttack(game, player_id, cards);
} 