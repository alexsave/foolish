import {wrap400, broadcastToGameUsers, verify_player_in_game, personalize_game, cardDisplay, validate_defender_status, verify_cards_in_players_hand, no_cards_left, check_win, card_comp, loadCompleteGame, saveCompleteGame } from "../_shared/utils.ts";
import { get_next_player_index } from "../_shared/common_utils.ts"; 
import { Game, Card, SERVER_EVENT_TYPE, PLAYER_STATUS, PrivatePlayer} from "../_shared/types.ts";

wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const { game_id, cards } = body;

    // Load complete game state using JOINs
    let game = await loadCompleteGame(game_id);

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Handle pass logic
    game = await handle_pass(game, game_id, user_id, cards);

    // Save complete game state back to separated tables
    await saveCompleteGame(game);

    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.PASS_PLAYED,
        message: `Player ${user_name} passed using ${cards.map(card => cardDisplay(card)).join(', ')}`
    });

    return {
        game: personalize_game(game, user_id)
    };

});

const handle_pass = async (game: Game, game_id: string, player_id: string, cards: Card[]): Promise<Game> => {
    if (!cards) {
        throw new Error(`No cards provided`);
    }
    const mCards = cards;

    // check if cards all have same value. 
    if (!mCards.every(card => card.value === mCards[0].value)) {
        throw new Error(`Cards ${mCards.map(card => cardDisplay(card)).join(', ')} are not all the same value`);
    }

    // check no duplicates
    if (new Set(mCards).size !== mCards.length) {
        throw new Error(`Cards ${mCards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // Find which player this is
    //const player = game.players.find(player => player.id === player_id)!;

    // also the attacker has to be the defender
    validate_defender_status(game, player_id, true);

    const defender: PrivatePlayer = game.players.find(player => player.player_id === player_id)!;

    verify_cards_in_players_hand(defender, mCards);

    // also important: THERE SHOULD BE CARDS ON THE TABLE
    if (game.table_battles.length === 0) {
        throw new Error(`No cards on the table`);
    }

    // check passability. 1. no cover, 2. all same value, 3. next player has enough cards
    // 1. no cover
    if (game.table_battles.some(battle => battle.defense !== null)) {
        throw new Error(`Cover present, cannot pass`);
    }
    // this also implies all same value on the table
    // and we already know that the pass cards are the same value
    // so check first pass card against all other cards on the tableo
    if (!game.table_battles.every(battle => battle.attack.value === mCards[0].value)) {
        throw new Error(`Cards ${mCards.map(card => cardDisplay(card)).join(', ')} do not match the values on the table`);
    }

    const next_player_index = get_next_player_index(game, game.defender);
    const next_player = game.players[next_player_index];
    if (next_player.hand.length < mCards.length + game.table_battles.length) {
        throw new Error(`Player ${next_player.name} does not have enough cards in their hand to cover ${mCards.map(card => cardDisplay(card)).join(', ')}`);
    }

    // Now we can pass
    // add to table
    //remove from hand
    // update defender

    for (const card of mCards) {
        game.table_battles.push({
            attack: card,
            defense: null
        });
    }
    defender.hand = defender.hand.filter(card => !mCards.some(mCard => card_comp(card, mCard)));


    // If the deck is empty, they can get out here
    if (no_cards_left(game) && defender.hand.length === 0) {
        // they win
        defender.status = PLAYER_STATUS.OUT;
        game.elimination_order.push(defender.player_id); // Track elimination order
        await check_win(game);
        game.defender = next_player_index;
        broadcastToGameUsers(game, 'game_update', {
            type: SERVER_EVENT_TYPE.PLAYER_WON,
            message: `Player ${player_id} passed ${mCards.map(card => cardDisplay(card)).join(', ')} and got rid of all their cards`,
        });
    } else {
        game.defender = next_player_index;

        broadcastToGameUsers(game, 'game_update', {
            type: SERVER_EVENT_TYPE.PASS_PLAYED,
            message: `Player ${player_id} used ${mCards.map(card => cardDisplay(card)).join(', ')} to pass to ${next_player.name}`,
        });
    }

    const uncovered_cards = game.table_battles.filter(battle => battle.defense === null).length;
    const new_defender_id = game.players[game.defender].player_id;
    // Lots of find calls. Maybe an intermediate type would be better for this
    const new_defender: PrivatePlayer = game.players.find(player => player.player_id === new_defender_id)!;
    const defender_cards = new_defender.hand.length;

    // it's important to check if we need to shift to only_defend
    if (uncovered_cards === defender_cards) {
        // just reached the limit
        game.status = 'only_defend';
        broadcastToGameUsers(game, 'game_update', {
            type: 'no_more_attacks',
            message: `Maximum number of attacks reached, only defender can defend`,
        });
    } else if (uncovered_cards > defender_cards) {
        // how the fuck did this happen
        throw new Error('Uncovered cards > defender_cards');
    } else if (uncovered_cards < defender_cards) {
        // a pass could shift from only_defend to free_play
        game.status = 'free_play';
        broadcastToGameUsers(game, 'game_update', {
            type: 'free_play_mode',
            message: `Passed cards, now free play mode`,
        });
    }

    return game;
}