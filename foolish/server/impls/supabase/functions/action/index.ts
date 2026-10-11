import { wrap400 } from "@shared/adapter/utils.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"

// Lazy so a cold start that only 404s or rejects never pulls the move path,
// but resolved ONCE: a dynamic import re-runs the resolver on every call.
const packedActionMod = (() => {
    let mod: Promise<typeof import('@shared/adapter/packed_action.ts')> | undefined;
    return () => (mod ??= import('@shared/adapter/packed_action.ts'));
})();

// A move (docs/C_GAME_SHAPE_MIGRATION.md 2.3), and nothing else: the body is the
// kernel's action request, the response the kernel's action response. `bump`,
// the bot-loop nudge this endpoint also took as a JSON object, is an ask now
// and goes to `meta` (c/src/ask.h ASK_BUMP).
wrap400(async ({ user, body, reqId }) => {
    const { executePackedAction } = await packedActionMod();
    const out = await executePackedAction(body, user.id, reqId);
    // A committed move that leaves a bot to act wakes the bot loop.
    return { body: out.body, runBots: out.needsBots ? out.gameId : null };
});
