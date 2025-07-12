import {wrap400, broadcastToGameUsers, loadCompleteGame, saveCompleteGame } from "../_shared/utils.ts";
import { Game, Card, SERVER_EVENT_TYPE } from "../_shared/types.ts";
import { handlePass } from "../_shared/actions/pass.ts";
import { verify_player_in_game, personalize_game, cardDisplay } from "../_shared/common_utils.ts";

wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const { game_id, cards } = body;

    // Load complete game state using JOINs
    let game = await loadCompleteGame(game_id);

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Handle pass logic
    await handlePass(game, user_id, cards);

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

