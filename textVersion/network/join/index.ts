import express from 'express';
import { wrap400, verify_game_id, lobbify_game, PLAYER_STATUS, GAME_STATUS, SERVER_EVENT_TYPE, database } from '../shared';


export const join = wrap400((req: express.Request, res: express.Response) => {
    const { games, users, player_games, public_game_channel } = database;

    const player_id = req.body.player_id;
    const game_id = verify_game_id(req.body.game_id);

    // check if player is already in game
    if (player_games[player_id] && player_games[player_id].includes(game_id)) {
        //throw new Error(`Player ${player_id} is already in game ${game_id}`);
    } else {
        games[game_id].players.push({
            name: users[player_id].name,
            id: player_id,
            status: PLAYER_STATUS.IDLE,
            hand: []
        });
        if (!player_games[player_id]) {
            player_games[player_id] = [];
        }
        player_games[player_id].push(game_id);
    }

    // check if game is ongoing
    if (games[game_id].status !== GAME_STATUS.WAITING) {
        throw new Error(`Game ${game_id} is not waiting`);
    }

    // send to all players in game
    public_game_channel.push({
        game_id: game_id,
        message: {
            type: SERVER_EVENT_TYPE.PLAYER_JOINED_GAME,
            message: `Player ${users[player_id].name} joined game ${game_id}`,
            game_id: game_id,
            game: games[game_id]
        }
    });

    res.end(JSON.stringify({
        game_id: game_id,
        game: lobbify_game(games[game_id])
    }));
});