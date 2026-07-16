import { wrap400, ExecutionParams, scheduleBotLoop } from "../_shared/adapter/utils.ts";
import { handleAttack } from "../_shared/common/actions/attack.ts";
import { handleCover } from "../_shared/common/actions/cover.ts";
import { handlePass } from "../_shared/common/actions/pass.ts";
import { handlePickup } from "../_shared/common/actions/pickup.ts";
import { handleGood } from "../_shared/common/actions/good.ts";
import { verify_player_in_game } from "../_shared/common/common_utils.ts";
import { corsHeaders } from "../_shared/adapter/cors.ts";
import { GAME_STATUS } from "../_shared/core/types.ts";
import { ACTION_STATUS, decodeActionRequest, encodeActionResponse } from "../_shared/sdk/ts/wire/awire.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"

// Packed fast path (docs/PACKED_WIRE_CUTOVER.md): the request body is the
// binary envelope [fmt | gid_len | game id | action wire] — the exact wire
// the client's guards.wasm validated. No JSON anywhere: the response is
// [fmt | status | reject_code | u32 version].
const packedAction = async (req: Request, user: { id: string }, reqId: string): Promise<Response> => {
    const body = new Uint8Array(await req.arrayBuffer());
    const parsed = decodeActionRequest(body);
    if (!parsed) {
        return new Response(JSON.stringify({ error: 'malformed action request' }), {
            status: 400, headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    }
    const { executePackedAction } = await import('../_shared/adapter/packed_action.ts');
    const out = await executePackedAction(parsed.gameId, user.id, parsed.wire, reqId, parsed.intentVersion);
    // Same bot nudge as the JSON path: an APPLIED human move wakes the bots.
    // (A rejection never did on the legacy path — it threw before run_bots.)
    if (out.status === ACTION_STATUS.APPLIED && out.gameStatus === GAME_STATUS.PLAYING) {
        scheduleBotLoop(parsed.gameId, reqId);
    }
    return new Response(encodeActionResponse(out.status, out.rejectCode, out.version) as unknown as BodyInit, {
        headers: { ...corsHeaders, 'Content-Type': 'application/octet-stream' },
    });
};

// Unified game endpoint. Replaces the five per-move edge functions (attack /
// cover / pass / pickup / good) AND the standalone bot_bump function with ONE
// function that dispatches on `body.type` — fewer Supabase functions => faster
// deploys. Each move's handler and request payload are unchanged; only the
// routing is consolidated.
//
// run_bots=true: wrap400 fires the (fire-and-forget) bot loop AFTER this returns,
// for every type. So a human move now also wakes the bots in the same request —
// previously the client had to make a separate bot_bump call. `type: "bump"` is
// the pure-nudge case (folded in from bot_bump): no move to apply, just trigger
// the loop.
wrap400(async ({ user, body, game }: ExecutionParams) => {
    const user_id = user.id;
    const { type } = body;

    // Pure bot-loop nudge (was the bot_bump function). Read-only and may be
    // triggered by a spectator's client, so it does NOT require player membership.
    if (type === "bump") {
        return { game, events: [] };
    }

    // Verify player is in game (shared by every real move)
    verify_player_in_game(game, user_id);

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
}, true, true, packedAction); // run_bots=true; mootIfGameOver=true (a move that lost the end-game race is a no-op, not a 400)
