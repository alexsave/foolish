import { wrap400, ExecutionParams } from "@shared/adapter/utils.ts";
import { handleMetaAction } from "@shared/adapter/meta_actions.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"

// Unified lobby / "game meta" endpoint. Replaces the separate functions
// (start / add-bot / exit / continue / join / rearrange-hand / rearrange-players /
// update-name) with ONE that dispatches on body.type — fewer Supabase functions
// => faster deploys. Mirrors how `action` consolidated the five per-move
// endpoints. Each move's logic is unchanged; only routing is consolidated.
// run_bots=true so a `start`/`add-bot` that dealt the game wakes the bots
// immediately when a bot is first attacker — previously nothing drove them
// until the 10s bot-heartbeat cron or a client bump. wrap400 only actually
// kicks the loop when the resulting game is PLAYING, so lobby-only actions
// (join / exit / rename / …) don't spawn one.
wrap400((params: ExecutionParams) => handleMetaAction(params), true);
