import express from 'express';
import { Game, wrap400, Card, verify_game_id, verify_player_in_game, database, personalize_game, GAME_STATUS, SERVER_EVENT_TYPE, PLAYER_STATUS, validate_defender_status, verify_hands_in_players_hand, cardDisplay, no_cards_left, check_win } from '../shared';

export const attack = wrap400((req: express.Request, res: express.Response) => {
    const { games } = database;

    const player_id = req.body.player_id;
    const game_id = verify_game_id(req.body.game_id);
    verify_player_in_game(game_id, player_id);

    handle_attack(games[game_id], game_id, player_id, req.body.cards);

    res.end(JSON.stringify({
        game_id: game_id,
        game: personalize_game(games[game_id], player_id)
    }));
});

const handle_attack = (game: Game, game_id: string, player_id: string, cards: Card[]) => {
    const { public_game_channel } = database;
    if (!cards) {
        throw new Error(`No cards provided`);
    }

    // check if cards all have same value. this is kinda iffy because you could put down multiple cards
    // at the same time as long as the values are on the board
    // But this also slows down attackign to make it more fair for all attackers
    if (!cards.every(card => card.value === cards[0].value)) {
        throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} are not all the same value`);
    }

    // check no duplicates
    if (new Set(cards).size !== cards.length) {
        throw new Error(`Cards ${cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // Find which player this is
    const player = game.players.find(player => player.id === player_id)!;

    // also the attacker cannot be the defender
    validate_defender_status(game, player_id, false);

    // check if every card is in hand
    verify_hands_in_players_hand(player, cards);

    // make sure there are enough cards in the defenders hand
    let uncovered_cards = game.table.filter(battle => battle.defense === null).length;
    let defender_cards = game.players[game.currentlyAttacked].hand.length;

    if (uncovered_cards + cards.length > defender_cards) {
        throw new Error(`Player ${player_id} does not have enough cards in their hand to cover ${cards.map(card => cardDisplay(card)).join(', ')}`);
    }

    if (game.status === GAME_STATUS.FIRST_ATTACKER) {
        // check if player is first attacker
        if (game.players[game.firstAttacker].id !== player.id) {
            throw new Error(`Player ${player_id} is not the first attacker`);
        }

        // Ok passed checks, we can put the cards on the table
        // remove from hand, put on table
        player.hand = player.hand.filter(card =>
            !cards.some(mCard => mCard.suit === card.suit && mCard.value === card.value));

        for (const card of cards) {
            game.table.push({
                attack: card,
                defense: null
            });
        }

        public_game_channel.push({
            game_id: game_id,
            message: {
                type: SERVER_EVENT_TYPE.ATTACK_PLAYED,
                message: `Player ${player_id} played ${cards.map(card => cardDisplay(card)).join(', ')}`,
                cards: cards,
                game: game
            }
        });

        // It's possible they win here
        if (no_cards_left(game) && player.hand.length === 0) {
            // they win
            player.status = PLAYER_STATUS.OUT;
            public_game_channel.push({
                game_id: game_id,
                message: {
                    type: 'player_wins',
                    message: `Player ${player_id} got rid of all their cards`
                }
            });
            check_win(game_id);
        }

        // Ok now that it's on the table, we set the status to "free for all"
        // Defender can pick up, cover, pass
        // All attackers can attack
        // Whatever comes in first comes first, otherwise gg
        game.status = GAME_STATUS.FREE_PLAY;


        // check win later, becuase a "first attack" could win, putting the game into idle
    } else if (game.status === GAME_STATUS.FREE_PLAY || game.status === GAME_STATUS.WAIT_FOR_ATTACKERS) {
        // This is very similar to the above, we just don't check if they are the first attacker
        // attack + free_play means you can do whatever

        // every value has to be on the table
        if (!cards.every(card => game.table.some(battle => battle.attack.value === card.value || battle.defense?.value === card.value))) {
            throw new Error(`Some card values of ${cards.map(card => cardDisplay(card)).join(', ')} are not on the table`);
        }
        // a valid attack will move us out of wait_for_attackers
        game.players.forEach(player => {
            if (player.status === PLAYER_STATUS.AWAITING_ATTACK) {
                player.status = PLAYER_STATUS.IN;
            }
        });
        game.status = GAME_STATUS.FREE_PLAY;

        player.hand = player.hand.filter(card =>
            !cards.some(mCard => mCard.suit === card.suit && mCard.value === card.value));
        for (const card of cards) {
            game.table.push({
                attack: card,
                defense: null
            });
        }

        public_game_channel.push({
            game_id: game_id,
            message: {
                type: SERVER_EVENT_TYPE.ATTACK_PLAYED,
                message: `Player ${player_id} played ${cards.map(card => cardDisplay(card)).join(', ')}`,
                cards: cards,
                game: game
            }
        });

        // It's possible they win here
        if (no_cards_left(game) && player.hand.length === 0) {
            // they win
            player.status = PLAYER_STATUS.OUT;
            public_game_channel.push({
                game_id: game_id,
                message: {
                    type: 'player_wins',
                    message: `Player ${player_id} got rid of all their cards`
                }
            });
            check_win(game_id);
        }

        uncovered_cards = game.table.filter(battle => battle.defense === null).length;
        defender_cards = game.players[game.currentlyAttacked].hand.length;

        // it's important to check if we need to shift to only_defend
        if (uncovered_cards === defender_cards) {
            // just reached the limit
            game.status = GAME_STATUS.ONLY_DEFEND;
            public_game_channel.push({
                game_id: game_id,
                message: {
                    type: 'no_more_attacks',
                    message: `Maximum number of attacks reached, only defender can defend`
                }
            });
        } else if (uncovered_cards > defender_cards) {
            // how the fuck did this happen
            throw new Error('SEVERE: Uncovered cards > defender_cards');
        }


    } else if (game.status === GAME_STATUS.ONLY_DEFEND) {
        // just reject
        throw new Error(`Player ${player_id} tried to attack but game is in only_defend mode`);
    } else {
        // handle others later
    }
}
