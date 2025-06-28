import express from 'express';
import { wrap400, verify_game_id, verify_player_in_game, getGames, getChatMessages, personalize_game } from '../shared';

export const chat = wrap400((req: express.Request, res: express.Response) => {
    const games = getGames();
    const chat_messages = getChatMessages();

    const player_id = req.body.player_id;
    const game_id = verify_game_id(req.body.game_id);
    verify_player_in_game(game_id, player_id);

    const message = req.body.message;
    const user_id = req.body.user_id;
    chat_messages.push({
        is_system: false,
        game_id,
        user_id,
        message,
    })

    res.end(JSON.stringify({
        game_id: game_id,
        game: personalize_game(games[game_id], player_id)
    }));
});
