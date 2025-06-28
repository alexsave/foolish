import express from 'express';
import { PLAYER_STATUS, card_comp, Card, wrap400, verify_game_id, verify_player_in_game, database, personalize_game, Game, GAME_STATUS, SERVER_EVENT_TYPE, validate_defender_status, refill, get_next_player_index, cardDisplay, verify_hands_in_players_hand, no_cards_left, check_win } from '../common';

export const pass = wrap400((req: express.Request, res: express.Response) => {
    const { games } = database;

    const player_id = req.body.player_id;
    const game_id = verify_game_id(req.body.game_id);
    verify_player_in_game(game_id, player_id);

    handle_pass(games[game_id], game_id, player_id, req.body.cards);

    res.end(JSON.stringify({
        game_id: game_id,
        game: personalize_game(games[game_id], player_id)
    }));

});

const handle_pass = (game: Game, game_id: string, player_id: string, cards: Card[]) => {
    const { public_game_channel } = database;

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
    const player = game.players.find(player => player.id === player_id)!;

    // also the attacker has to be the defender
    validate_defender_status(game, player_id, true);

    verify_hands_in_players_hand(player, mCards);

    // also important: THERE SHOULD BE CARDS ON THE TABLE
    if (game.table.length === 0) {
        throw new Error(`No cards on the table`);
    }

    // check passability. 1. no cover, 2. all same value, 3. next player has enough cards
    // 1. no cover
    if (game.table.some(battle => battle.defense !== null)) {
        throw new Error(`Cover present, cannot pass`);
    }
    // this also implies all same value on the table
    // and we already know that the pass cards are the same value
    // so check first pass card against all other cards on the tableo
    if (!game.table.every(battle => battle.attack.value === mCards[0].value)) {
        throw new Error(`Cards ${mCards.map(card => cardDisplay(card)).join(', ')} do not match the values on the table`);
    }

    const next_player_index = get_next_player_index(game, game.currentlyAttacked);
    const next_player = game.players[next_player_index];
    if (next_player.hand.length < mCards.length + game.table.length) {
        throw new Error(`Player ${next_player.name} does not have enough cards in their hand to cover ${mCards.map(card => cardDisplay(card)).join(', ')}`);
    }

    // Now we can pass
    // add to table
    //remove from hand
    // update currentlyAttacked

    for (const card of mCards) {
        game.table.push({
            attack: card,
            defense: null
        });
    }
    player.hand = player.hand.filter(card => !mCards.some(mCard => card_comp(card, mCard)));


    // If the deck is empty, they can get out here
    if (no_cards_left(game) && player.hand.length === 0) {
        // they win
        player.status = PLAYER_STATUS.OUT;
        public_game_channel.push({
            game_id: game_id,
            message: {
                type: 'player_wins',
                message: `Player ${player_id} got rid of all their cards`,
                game: game
            }
        });
        /*broadcast_to_game(game_id, {
            type: 'player_wins',
            message: `Player ${player_id} got rid of all their cards`
        });*/
        check_win(game_id);
    }

    game.currentlyAttacked = next_player_index;


    public_game_channel.push({
        game_id: game_id,
        message: {
            type: SERVER_EVENT_TYPE.PASS_PLAYED,
            message: `Player ${player_id} used ${mCards.map(card => cardDisplay(card)).join(', ')} to pass to ${next_player.name}`,
            cards: mCards,
            game: game
        }
    });

    const uncovered_cards = game.table.filter(battle => battle.defense === null).length;
    const defender_cards = game.players[game.currentlyAttacked].hand.length;

    // it's important to check if we need to shift to only_defend
    if (uncovered_cards === defender_cards) {
        // just reached the limit
        game.status = 'only_defend';
        public_game_channel.push({
            game_id: game_id,
            message: {
                type: 'no_more_attacks',
                message: `Maximum number of attacks reached, only defender can defend`,
                game: game
            }
        });
    } else if (uncovered_cards > defender_cards) {
        // how the fuck did this happen
        throw new Error('Uncovered cards > defender_cards');
    } else if (uncovered_cards < defender_cards) {
        // a pass could shift from only_defend to free_play
        game.status = 'free_play';
        public_game_channel.push({
            game_id: game_id,
            message: {
                type: 'free_play_mode',
                message: `Passed cards, now free play mode`,
                game: game
            }
        });
    }
}