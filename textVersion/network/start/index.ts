import { wrap400, verify_game_id, start_game, verify_player_in_game, lobbify_game, PLAYER_STATUS, GAME_STATUS, SERVER_EVENT_TYPE } from '../common';
import express from 'express';
import { database } from '../common';

export const start = wrap400((req: express.Request, res: express.Response) => {
    const { games, users, player_games, public_game_channel } = database;

    const player_id = req.body.player_id;
    const game_id = verify_game_id(req.body.game_id);

    // user wants to start a game. switch them to ready and see if all other players are ready. and if tehre are 2+ players

    verify_player_in_game(game_id, player_id);

    const game = games[game_id];

    // check if game is waiting
    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Game ${game_id} is not waiting`);
    }

    // set player to ready
    game.players.find(player => player.id === player_id)!.status = PLAYER_STATUS.READY;

    // two events I know
    public_game_channel.push({
        game_id: game_id,
        message: {
            type: SERVER_EVENT_TYPE.PLAYER_READY,
            message: `Player ${users[player_id].name} is ready`,
            game_id: game_id,
            game: games[game_id]
        }
    });

    // check if all players are ready
    if (game.players.length >= 2 &&
        game.players.every(player => player.status === PLAYER_STATUS.READY)) {
        start_game(game_id);

        // send to all players in game
        public_game_channel.push({
            game_id: game_id,
            message: {
                type: SERVER_EVENT_TYPE.GAME_STARTED,
                message: `Game ${game_id} started`,
                game_id: game_id,
                game: games[game_id]
            }
        });

    }
    res.end(JSON.stringify({
        game_id: game_id,
        game: lobbify_game(games[game_id])
    }));
});