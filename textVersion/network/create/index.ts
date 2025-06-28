import express from 'express';
import { GAME_STATUS, PLAYER_STATUS, wrap400, createId, lobbify_game, getGames, getUsers, getPlayerGames, getPublicGameChannel } from '../shared';

export const create = wrap400((req: express.Request, res: express.Response) => {

    const games = getGames();
    const users = getUsers();
    const player_games = getPlayerGames();
    const public_game_channel = getPublicGameChannel();

    // Every request should probably have a player_id now
    const player_id = req.body.player_id;

    const game_id = createId();
    games[game_id] = {
        status: GAME_STATUS.WAITING,
        players: [{
            name: users[player_id].name,
            id: player_id,
            status: PLAYER_STATUS.IDLE,
            hand: []
        }],
        deck: [],
        flipped: null,
        powerSuit: 0,
        firstAttacker: 0,
        currentlyAttacked: 0,
        previousFirstAttacker: 0,
        previousCurrentlyAttacked: 0,
        table: []
    }
    if (!player_games[player_id]) {
        player_games[player_id] = [];
    }
    player_games[player_id].push(game_id);

    // Doesn't REALLY make sense to send this, because the only thing that happens is the player joins the game.
    // If they're the ones creating it, they're the only ones in it. But they already know the game
    public_game_channel.push({
        game_id: game_id,
        message: {
            type: 'game_created',
            message: `Game created with id ${game_id}`,
            game_id: game_id
        }
    });

    res.end(JSON.stringify({
        game_id: game_id,
        game: lobbify_game(games[game_id])
    }));
});