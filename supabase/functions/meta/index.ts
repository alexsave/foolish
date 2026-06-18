import { wrap400, ExecutionParams } from "../_shared/utils.ts";
import { handleMetaAction } from "../_shared/meta_actions.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"

// Unified lobby / "game meta" endpoint. Replaces the four separate functions
// (start / add-bot / exit / continue) with ONE that dispatches on body.type —
// fewer Supabase functions => faster deploys. Mirrors how `action` consolidated
// the five per-move endpoints. Each move's logic is unchanged; only routing is
// consolidated. run_bots=false (these are lobby/meta operations; a bot drive is
// kicked by the gameplay `action` path).
wrap400((params: ExecutionParams) => handleMetaAction(params), false);
