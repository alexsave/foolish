import { wrap400 } from "@shared/adapter/utils.ts";
import { handleMetaAction } from "@shared/adapter/meta_actions.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"

// The lobby / "game meta" endpoint: every request that is not a move. The body
// is an ask (c/src/ask.h: join, start, add-bot, exit, continue, rearrange-hand,
// rearrange-players, update-name, set-rules, and bump, the bot-loop nudge),
// which the kernel reads and runs as one C Table operation (meta_actions.ts).
// An edit that deals a table with a bot to move wakes the bot loop straight away.
wrap400(handleMetaAction);
