import express from 'express';
import { wrap400, verify_game_id, verify_player_in_game, database, personalize_game, Game, GAME_STATUS, SERVER_EVENT_TYPE, validate_defender_status, refill, get_next_player_index, card_comp } from '../common';

export const pickup = wrap400((req: express.Request, res: express.Response) => {
    const { games } = database;
    const player_id = req.body.player_id;
    const game_id = verify_game_id(req.body.game_id);
    verify_player_in_game(game_id, player_id);

    handle_pickup(games[game_id], game_id, player_id);

    res.end(JSON.stringify({
        game_id: game_id,
        game: personalize_game(games[game_id], player_id)
    }));

});

const handle_pickup = (game: Game, game_id: string, player_id: string) => {
    const { public_game_channel } = database;

    // pick up a card

    if (game.status !== GAME_STATUS.FREE_PLAY && game.status !== GAME_STATUS.ONLY_DEFEND) {
        throw new Error(`Game ${game_id} is not in free_play or only_defend mode`);
    }

    // check if player is the defender
    validate_defender_status(game, player_id, true);
    // TODO add a timer + check to make sure they don't pick up too quickly

    // check if there are cards on the table
    if (game.table.length === 0) {
        throw new Error(`No cards on the table`);
    }

    // ok let's just pick it up

    // add cards from table to hand
    game.table.forEach(battle => {
        game.players[game.currentlyAttacked].hand.push(battle.attack);
        if (battle.defense) {
            game.players[game.currentlyAttacked].hand.push(battle.defense);
        }
    });


    // clear table
    game.table = [];

    public_game_channel.push({
        game_id: game_id,
        message: {
            type: SERVER_EVENT_TYPE.PICKUP_PLAYED,
            message: `Player ${player_id} picked up cards`,
            game: game
        }
    });

    // Draw cards starting from first attacker

    refill(game_id);

    // shift
    game.firstAttacker = get_next_player_index(game, game.currentlyAttacked);
    game.currentlyAttacked = get_next_player_index(game, game.firstAttacker);
    game.status = GAME_STATUS.FIRST_ATTACKER;
}