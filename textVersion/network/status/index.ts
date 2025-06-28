import express from 'express';
import { wrap400, verify_game_id, verify_player_in_game, getGames, personalize_game } from '../shared';

export const status = wrap400((req: express.Request, res: express.Response) => {
    const games = getGames();

    const player_id = req.body.player_id;
    const game_id = verify_game_id(req.body.game_id);
    verify_player_in_game(game_id, player_id);

    res.end(JSON.stringify({
        game_id: game_id,
        game: personalize_game(games[game_id], player_id)
    }));
});
