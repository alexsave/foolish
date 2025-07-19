import {wrap400 } from "../_shared/utils.ts";
import { SERVER_EVENT_TYPE } from "../_shared/types.ts";
import { handlePass } from "../_shared/actions/pass.ts";
import { verify_player_in_game, cardDisplay } from "../_shared/common_utils.ts";

wrap400(async (user, user_name, body, game) => {
    const user_id = user.id;
    const { cards } = body;

    // Load complete game state using JOINs
    //let game = await loadCompleteGame(game_id);

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Handle pass logic and return animation events
    const events = await handlePass(game, user_id, cards);

    return { game, events };

}, true);

