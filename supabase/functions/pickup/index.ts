import { wrap400, ExecutionParams } from "../_shared/utils.ts";
import { handlePickup } from "../_shared/actions/pickup.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";

wrap400(async ({user, game}: ExecutionParams) => {
    const user_id = user.id;

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Handle pickup logic and return animation events
    const events = await handlePickup(game, user_id);

    return { game, events };
}, false);

