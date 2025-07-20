import { wrap400, ExecutionParams } from "../_shared/utils.ts";
import { handleAttack } from "../_shared/actions/attack.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"

wrap400(async ({user, body, game}: ExecutionParams) => {
    const user_id = user.id;
    const { cards } = body;

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Handle attack logic and return animation events
    const events = await handleAttack(game, user_id, cards);

    return { game, events };
}, true);

