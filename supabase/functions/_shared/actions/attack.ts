import { Card, Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, SERVER_EVENT_TYPE, AnimationEvent, ANIMATION_EVENT_TYPE } from '../types.ts';
import { executeWithGameLock, check_win, broadcastToGameUsers } from '../utils.ts';
import { validate_defender_status, verify_cards_in_players_hand, no_cards_left, cardDisplay, card_comp } from '../common_utils.ts';
import { lockedBotLoop } from '../bot_actions.ts';

// Validation function for attack moves
export function validateAttack(game: Game, player_id: string, cards: Card[]): void {
    if (!cards || cards.length === 0) {
        throw new Error('No cards provided');
    }

    if (game.status !== GAME_STATUS.FIRST_ATTACKER && game.status !== GAME_STATUS.FREE_PLAY && game.status !== GAME_STATUS.WAIT_FOR_ATTACKERS) {
        throw new Error(`Game ${game.id} is not in valid attack state`);
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

    // Basic attack validation based on game state
    if (game.status === GAME_STATUS.FIRST_ATTACKER) {
        // First attacker must play cards of same value
        if (!cards.every(card => card.value === cards[0].value)) {
            throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} are not all the same value`);
        }
        
        // Check if player is first attacker
        if (game.players[game.first_attacker].player_id !== player_id) {
            throw new Error(`Player ${player_id} is not the first attacker`);
        }
    } else if (game.status === GAME_STATUS.FREE_PLAY || game.status === GAME_STATUS.WAIT_FOR_ATTACKERS) {
        // Every value has to be on the table
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

    // Update game state based on current status
    if (game.status === GAME_STATUS.FIRST_ATTACKER) {
        game.status = GAME_STATUS.FREE_PLAY;
        
        // Set awaiting_attack for all players except defender
        game.players.forEach((player, index) => {
            if (index !== game.defender) {
                player.awaiting_attack = true;
            }
        });
    } else if (game.status === GAME_STATUS.FREE_PLAY) {
        // Already in free play, attacker continues to await
        attacker.awaiting_attack = true;
    } else if (game.status === GAME_STATUS.WAIT_FOR_ATTACKERS) {
        // Player can attack in this state
        attacker.awaiting_attack = true;
    }

    return events;
}

// Combined function with validation
export async function handleAttack(game: Game, player_id: string, cards: Card[]): Promise<AnimationEvent[]> {
    validateAttack(game, player_id, cards);
    return await executeAttack(game, player_id, cards);
} 