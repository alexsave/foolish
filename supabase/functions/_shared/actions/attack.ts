import { Card, Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS } from '../types.ts';
import { check_win } from '../utils.ts';
import { validate_defender_status, verify_cards_in_players_hand, no_cards_left, cardDisplay } from '../common_utils.ts';

// Validation function for attack moves
export function validateAttack(game: Game, player_id: string, cards: Card[]): void {
    if (!cards) {
        throw new Error(`No cards provided`);
    }

    // check no duplicates
    if (new Set(cards).size !== cards.length) {
        throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // Find which player this is
    const player: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;

    // also the attacker cannot be the defender
    validate_defender_status(game, player_id, false);

    // check if every card is in hand
    verify_cards_in_players_hand(player, cards);

    // make sure there are enough cards in the defenders hand
    let uncovered_cards = game.table_battles.filter(battle => battle.defense === null).length;
    const defender: PrivatePlayer = game.players[game.defender];
    let defender_cards = defender.hand.length;

    if (uncovered_cards + cards.length > defender_cards) {
        throw new Error(`Player ${player_id} does not have enough cards in their hand to cover ${cards.map(card => cardDisplay(card)).join(', ')}`);
    }

    if (game.status === GAME_STATUS.FIRST_ATTACKER) {
        // check if cards all have same value
        if (!cards.every(card => card.value === cards[0].value)) {
            throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} are not all the same value`);
        }

        // check if player is first attacker
        if (game.players[game.first_attacker].player_id !== player_id) {
            throw new Error(`Player ${player_id} is not the first attacker`);
        }
    } else if (game.status === GAME_STATUS.FREE_PLAY || game.status === GAME_STATUS.WAIT_FOR_ATTACKERS) {
        // every value has to be on the table
        if (!cards.every(card => game.table_battles.some(battle => battle.attack.value === card.value || battle.defense?.value === card.value))) {
            throw new Error(`Some card values of ${cards.map(card => cardDisplay(card)).join(', ')} are not on the table`);
        }
    } else {
        throw new Error(`Player ${player_id} tried to attack but game is not in valid state`);
    }
}

// Execution function for attack moves
export async function executeAttack(game: Game, player_id: string, cards: Card[]): Promise<void> {
    const player: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;
    const defender: PrivatePlayer = game.players[game.defender];

    if (game.status === GAME_STATUS.FIRST_ATTACKER) {
        // remove from hand, put on table
        player.hand = player.hand.filter(card =>
            !cards.some(mCard => mCard.suit === card.suit && mCard.value === card.value));

        for (const card of cards) {
            game.table_battles.push({
                attack: card,
                defense: null
            });
        }

        if (no_cards_left(game) && player.hand.length === 0) {
            player.status = PLAYER_STATUS.OUT;
            game.elimination_order.push(player.player_id);
            await check_win(game);
        }

        game.status = GAME_STATUS.FREE_PLAY;

    } else if (game.status === GAME_STATUS.FREE_PLAY || game.status === GAME_STATUS.WAIT_FOR_ATTACKERS) {
        // a valid attack will move us out of wait_for_attackers
        game.players.forEach(player => {
            if (player.awaiting_attack) {
                player.status = PLAYER_STATUS.IN;
            }
        });
        game.status = GAME_STATUS.FREE_PLAY;

        player.hand = player.hand.filter(card =>
            !cards.some(mCard => mCard.suit === card.suit && mCard.value === card.value));
        for (const card of cards) {
            game.table_battles.push({
                attack: card,
                defense: null
            });
        }

        if (no_cards_left(game) && player.hand.length === 0) {
            player.status = PLAYER_STATUS.OUT;
            game.elimination_order.push(player.player_id);
            await check_win(game);
        }

        const uncovered_cards = game.table_battles.filter(battle => battle.defense === null).length;
        const defender_cards = defender.hand.length;

        if (uncovered_cards === defender_cards) {
            game.status = GAME_STATUS.ONLY_DEFEND;
        } else if (uncovered_cards > defender_cards) {
            throw new Error('SEVERE: Uncovered cards > defender_cards');
        }
    }
}

// Combined function with validation
export async function handleAttack(game: Game, player_id: string, cards: Card[]): Promise<void> {
    validateAttack(game, player_id, cards);
    await executeAttack(game, player_id, cards);
} 