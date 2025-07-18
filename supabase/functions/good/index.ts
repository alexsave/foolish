import { wrap400, broadcastToGameUsers } from "../_shared/utils.ts";
import { handleGood } from "../_shared/actions/good.ts";
import { SERVER_EVENT_TYPE } from "../_shared/types.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";

wrap400(async (user, user_name, body, game) => {
    const user_id = user.id;

    // Load complete game state using JOINs
    //let game = await loadCompleteGame(game_id);

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Handle good logic
    await handleGood(game, user_id);

    // Save complete game state back to separated tables
    //await saveCompleteGame(game);

    // we still need to send a broadcast in case the good causes the game to transition
    // but we can't let other players know that good was played
    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.GOOD_PLAYED,
        message: `Player ${user_id} played good`
    });

    //return {
    //    game: personalize_game(game, user_id)
    //};
}, true);

