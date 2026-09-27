import { wrap400 } from "@shared/adapter/utils.ts";
import { handleMetaAction, prefetchBots } from "@shared/adapter/meta_actions.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"

// The lobby / "game meta" endpoint: one function that dispatches on body.type
// (start, add-bot, exit, continue, join, rearrange-hand, rearrange-players,
// update-name), each a C Table operation (meta_actions.ts). An edit that deals
// a table with a bot to move wakes the bot loop straight away.
//
// add-bot's bots read starts here, before the game load, so the two overlap.
wrap400((ctx) => handleMetaAction(ctx, prefetchBots(ctx.body)));
