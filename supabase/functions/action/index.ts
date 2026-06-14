import { wrap400, ExecutionParams } from "../_shared/utils.ts";
import { handleAttack } from "../_shared/actions/attack.ts";
import { handleCover } from "../_shared/actions/cover.ts";
import { handlePass } from "../_shared/actions/pass.ts";
import { handlePickup } from "../_shared/actions/pickup.ts";
import { handleGood } from "../_shared/actions/good.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"

// Unified game-move endpoint. Replaces the five per-move edge functions
// (attack / cover / pass / pickup / good) with ONE function that dispatches on
// `body.type` — fewer Supabase functions => faster deploys. Each move's handler
// and request payload are unchanged; only the routing is consolidated.
wrap400(async ({ user, body, game }: ExecutionParams) => {
    const user_id = user.id;

    // Verify player is in game (shared by every move)
    verify_player_in_game(game, user_id);

    const { type } = body;
    let events;
    switch (type) {
        case "attack":
            events = await handleAttack(game, user_id, body.cards);
            break;
        case "cover":
            events = await handleCover(game, user_id, body.cover_cards, body.attack_cards);
            break;
        case "pass":
            events = await handlePass(game, user_id, body.cards);
            break;
        case "pickup":
            events = await handlePickup(game, user_id);
            break;
        case "good":
            events = await handleGood(game, user_id);
            break;
        default:
            throw new Error(`unknown action type: ${type}`);
    }

    return { game, events };
}, false);
