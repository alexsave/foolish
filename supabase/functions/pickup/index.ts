import { wrap400, broadcastToGameUsers, loadCompleteGame, saveCompleteGame } from "../_shared/utils.ts";
import { SERVER_EVENT_TYPE } from "../_shared/types.ts";
import { handlePickup } from "../_shared/actions/pickup.ts";
import { verify_player_in_game, personalize_game } from "../_shared/common_utils.ts";

wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const { game_id } = body;

    // Load complete game state using JOINs
    let game = await loadCompleteGame(game_id);

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Handle pickup logic
    await handlePickup(game, user_id);

    // Save complete game state back to separated tables
    await saveCompleteGame(game);

    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.PICKUP_PLAYED,
        message: `Player ${user_id} picked up cards`
    });

    return {
        game: personalize_game(game, user_id)
    };
});

