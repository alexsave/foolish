import { wrap400, broadcastToGameUser, ExecutionParams } from "../_shared/utils.ts";
import { SERVER_EVENT_TYPE } from "../_shared/types.ts";
import { handleRearrangeHand } from "../_shared/actions/rearrange.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"

wrap400(async ({user, user_name, body, game}: ExecutionParams) => {
    const user_id = user.id;

    // Reorder the caller's hand. Validates membership + that card_indices is a
    // permutation (the uniqueness check prevents a client from minting duplicate
    // cards via repeated indices). Mutates game in place.
    handleRearrangeHand(game, user_id, body.card_indices);

    // This doesn't need a broadcast to everyone
    broadcastToGameUser(game, SERVER_EVENT_TYPE.HAND_REARRANGED, {
        message: `${user_name} rearranged their hand`
    }, user_id);

    return { game, events: [] };
}, false);
