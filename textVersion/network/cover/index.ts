import express from 'express';
import { PLAYER_STATUS,Card, wrap400, canCover, card_comp,verify_game_id, verify_player_in_game, getGames, getPublicGameChannel, getPrivateUserChannel, personalize_game, Game, GAME_STATUS, SERVER_EVENT_TYPE, validate_defender_status, refill, get_next_player_index, cardDisplay, verify_hands_in_players_hand, check_win } from '../shared';

export const cover = wrap400((req: express.Request, res: express.Response) => {

    const games = getGames();
    const player_id = req.body.player_id
    const game_id = verify_game_id(req.body.game_id);
    verify_player_in_game(game_id, player_id);

    handle_cover(games[game_id], game_id, player_id, req.body.cover_cards, req.body.attack_cards);

    res.end(JSON.stringify({
        game_id: game_id,
        game: personalize_game(games[game_id], player_id)
    }));
});

const handle_cover = (game: Game, game_id: string, player_id: string, cover_cards: Card[], attack_cards: Card[]) => {
    const public_game_channel = getPublicGameChannel();
    const private_user_channel = getPrivateUserChannel();
    // cover a card


    if (game.status !== GAME_STATUS.FREE_PLAY && game.status !== GAME_STATUS.ONLY_DEFEND) {
        throw new Error(`Game ${game_id} is not in free_play or only_defend mode`);
    }

    // check if player is the defender
    validate_defender_status(game, player_id, true);

    // ok now for the fun part
    // how should we handle this?
    // ok there's going to be 2 arrays of cards
    // the first one is the cards that are being covered
    // the second one is the cards that are being used to cover



    // ok first just make sure all the cards are in the hand
    verify_hands_in_players_hand(game.players[game.currentlyAttacked], cover_cards);

    // check no duplicates
    if (new Set(cover_cards).size !== cover_cards.length) {
        throw new Error(`Cards ${cover_cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }


    // ensure that each of the attack cards are on the table AND uncovered
    for (const card of attack_cards) {
        if (!game.table.some(battle => battle.attack.value === card.value && battle.defense === null)) {
            throw new Error(`Card ${cardDisplay(card)} is not on the table`);
        }
    }

    // check no duplicates
    if (new Set(attack_cards).size !== attack_cards.length) {
        throw new Error(`Cards ${attack_cards.map(card => cardDisplay(card)).join(', ')} have duplicates`);
    }

    // ok now we know that the cards are in the hand and on the table
    // can they cover?
    for (let i = 0; i < cover_cards.length; i++) {
        const cover_card = cover_cards[i];
        const attack_card = attack_cards[i];
        if (!canCover(attack_card, cover_card, game.powerSuit)) {
            throw new Error(`Card ${cardDisplay(cover_card)} cannot cover ${cardDisplay(attack_card)}`);
        }
    }

    // assert same size of arrays
    if (cover_cards.length !== attack_cards.length) {
        throw new Error(`Cover cards ${cover_cards.map(card => cardDisplay(card)).join(', ')} and attack cards ${attack_cards.map(card => cardDisplay(card)).join(', ')} have different sizes`);
    }

    // now cover the cards
    for (let i = 0; i < cover_cards.length; i++) {
        const cover_card = cover_cards[i];
        const attack_card = attack_cards[i];
        // find the attack card on the table
        const attack_card_index = game.table.findIndex(battle => card_comp(battle.attack, attack_card) && battle.defense === null);
        if (attack_card_index === -1) {
            // This shouldn't happen as we just validated
            throw new Error('SEVERE: Card not found on table');
        }
        game.table[attack_card_index].defense = cover_card;
        public_game_channel.push({
            game_id: game_id,
            message: {
                type: SERVER_EVENT_TYPE.COVER_PLAYED,
                message: `Player ${player_id} covered ${cardDisplay(attack_card)} with ${cardDisplay(cover_card)}`,
                game: game
            }
        });
        // remove the cards from the hand
        //game.players[game.currentlyAttacked].hand = game.players[game.currentlyAttacked].hand.filter(card => !card_comp(card, cover_card));

    }

    // remove the cards from the hand
    game.players[game.currentlyAttacked].hand = game.players[game.currentlyAttacked].hand.filter(card => !cover_cards.some(cover_card => card_comp(card, cover_card)));

    // There is one scenario where we instantly move on: the player has no cards left in their hand
    if (game.players[game.currentlyAttacked].hand.length === 0) {

        game.table = []; // burn the cards. TODO keep track of HOW MANY cards are burned but not which
        refill(game_id);
        // and it's fucking tricky because they can win here
        // shift 
        game.firstAttacker = game.currentlyAttacked;
        if (game.players[game.firstAttacker].hand.length === 0) {
            // can't think right now, but we need better win checking 
            public_game_channel.push({
                game_id: game_id,
                message: {
                    type: SERVER_EVENT_TYPE.PLAYER_WON,
                    message: `Player ${game.players[game.firstAttacker].name} got rid of their hand`,
                    game: game
                }
            });
            // win if still empty after refill
            game.players[game.firstAttacker].status = PLAYER_STATUS.OUT;
            check_win(game_id);
            game.firstAttacker = get_next_player_index(game, game.firstAttacker);
        }
        game.currentlyAttacked = get_next_player_index(game, game.firstAttacker);
        return;
    }

    // only do this if all table cards are covered but the defender has cards left
    // we know they have cards left
    const all_attacks_covered = game.table.every(battle => battle.defense !== null);
    if (all_attacks_covered) {

        // so in the real game, we would give it like 15seconds to let other people throw down cards.
        // because this will be offline, we give them infinite time. 
        // to proceed the next round, do we need all players to agree? but it should be done in secret to avoid revealing values
        // Yeah I don't know how to make it not obvious that we're waiting for attackers because they have cards
        // Oh well
        game.status = GAME_STATUS.WAIT_FOR_ATTACKERS;

        // ok let's secretly see who can even play cards.
        // pretty simple. Because they just covered, the only cards that can be played are values on teh table
        const playable_values = new Set<number>();
        for (const battle of game.table) {
            playable_values.add(battle.attack.value)
            if (battle.defense !== null) {
                playable_values.add(battle.defense.value);
            }
        }

        // now we need to see who can play cards. not the defender lol
        const playable_players = game.players.filter(player => player.id !== game.players[game.currentlyAttacked].id && player.hand.some(card => playable_values.has(card.value)));
        if (playable_players.length === 0) {
            // no one can play cards
            // but don't make it that obvious. give it 30 seconds
            setTimeout(() => {
                //shift 
                public_game_channel.push({
                    game_id: game_id,
                    message: {
                        type: SERVER_EVENT_TYPE.SUCCESSFULLY_COVERED,
                        message: `Player ${player_id} successfully defended the attack`,
                        game: game
                    }
                });
                game.table = [];
                refill(game_id);
                game.firstAttacker = game.currentlyAttacked;
                game.currentlyAttacked = get_next_player_index(game, game.firstAttacker);
                game.status = GAME_STATUS.FIRST_ATTACKER;
            }, 5000 + Math.random() * 20000);
        } else {
            // someone can play cards
            // so we need to see who can play cards
            playable_players.forEach(player => {
                player.status = PLAYER_STATUS.AWAITING_ATTACK;
            });

            playable_players.forEach(player => {
                private_user_channel.push({
                    user_id: player.id,
                    message: {
                        type: SERVER_EVENT_TYPE.PLAYABLE_CARDS,
                        message: `You can still play cards. Either play or confirm you are done attacking with "good"`,
                        game: game
                    }
                });
                // send them a message
                /*user_ports[player.id].send(JSON.stringify({
                    type: 'playable_cards',
                    message: `You can still play cards. Either play or confirm you are done attacking with "good"`
                }));*/
            });

        }
    }

}