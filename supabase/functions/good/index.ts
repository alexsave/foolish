import { ExecutionParams, wrap400 } from "../_shared/utils.ts";
import { handleGood } from "../_shared/actions/good.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";

wrap400(async ({user, game}: ExecutionParams) => {
    const user_id = user.id;

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Handle good logic and return animation events
    const events = await handleGood(game, user_id);

    return { game, events };
}, true);

