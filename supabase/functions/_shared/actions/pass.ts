import { Card, Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS } from '../types.ts';
import { check_win } from '../utils.ts';
import { get_next_player_index, validate_defender_status, verify_cards_in_players_hand, no_cards_left, card_comp, cardDisplay } from '../common_utils.ts';

// Validation function for pass moves
export function validatePass(game: Game, player_id: string, cards: Card[]): void {
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
export async function executePass(game: Game, player_id: string, cards: Card[]): Promise<void> {
    const defender: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;

    // Add to table and remove from hand
    for (const card of cards) {
        game.table_battles.push({
            attack: card,
            defense: null
        });
    }
    defender.hand = defender.hand.filter(card => !cards.some(mCard => card_comp(card, mCard)));

    const next_player_index = get_next_player_index(game, game.defender);

    // If the deck is empty, they can get out here
    if (no_cards_left(game) && defender.hand.length === 0) {
        defender.status = PLAYER_STATUS.OUT;
        game.elimination_order.push(defender.player_id);
        await check_win(game);
        game.defender = next_player_index;
    } else {
        game.defender = next_player_index;
    }

    const uncovered_cards = game.table_battles.filter(battle => battle.defense === null).length;
    const new_defender: PrivatePlayer = game.players[game.defender];
    const defender_cards = new_defender.hand.length;

    // Check game status
    if (uncovered_cards === defender_cards) {
        game.status = GAME_STATUS.ONLY_DEFEND;
    } else if (uncovered_cards > defender_cards) {
        throw new Error('Uncovered cards > defender_cards');
    } else if (uncovered_cards < defender_cards) {
        game.status = GAME_STATUS.FREE_PLAY;
    }
}

// Combined function with validation
export async function handlePass(game: Game, player_id: string, cards: Card[]): Promise<void> {
    validatePass(game, player_id, cards);
    await executePass(game, player_id, cards);
} 