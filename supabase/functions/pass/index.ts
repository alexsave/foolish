import {ExecutionParams, wrap400 } from "../_shared/utils.ts";
import { handlePass } from "../_shared/actions/pass.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";

wrap400(async ({user, body, game}: ExecutionParams) => {
    const user_id = user.id;
    const { cards } = body;

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Handle pass logic and return animation events
    const events = await handlePass(game, user_id, cards);

    return { game, events };

}, false);

