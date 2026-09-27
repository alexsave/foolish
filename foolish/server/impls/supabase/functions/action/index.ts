import { wrap400, scheduleBotLoop } from "@shared/adapter/utils.ts";
import { corsHeaders } from "@shared/adapter/cors.ts";
import { loadRow } from "@shared/adapter/table_io.ts";
import { serverTable } from "@sdk/ts/table/server_table.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"

// Lazy so a cold start that only 404s or rejects never pulls the move path,
// but resolved ONCE: a dynamic import re-runs the resolver on every call.
const packedActionMod = (() => {
    let mod: Promise<typeof import('@shared/adapter/packed_action.ts')> | undefined;
    return () => (mod ??= import('@shared/adapter/packed_action.ts'));
})();

// Packed moves (docs/C_GAME_SHAPE_MIGRATION.md 2.3): the body is the kernel's
// action request, the response the kernel's action response. No JSON.
const packedAction = async (req: Request, user: { id: string }, reqId: string): Promise<Response> => {
    const body = new Uint8Array(await req.arrayBuffer());
    const { executePackedAction, MalformedActionRequest } = await packedActionMod();
    let out;
    try {
        out = await executePackedAction(body, user.id, reqId);
    } catch (e) {
        if (e instanceof MalformedActionRequest) {
            return new Response(JSON.stringify({ error: e.message }), {
                status: 400, headers: { ...corsHeaders, 'Content-Type': 'application/json' },
            });
        }
        throw e;
    }
    // A committed move that leaves a bot to act wakes the bot loop.
    if (out.needsBots) scheduleBotLoop(out.gameId, reqId);
    return new Response(out.body as unknown as BodyInit, {
        headers: { ...corsHeaders, 'Content-Type': 'application/octet-stream' },
    });
};

// The JSON side of `action` is one request: `bump`, the bot-loop nudge. It may
// come from a spectator's client, so it needs no seat: it commits nothing,
// answers with the caller's envelope (the spectator's when not seated), and
// wakes the bots when the kernel says the table has bot work. A JSON move is
// refused: moves are packed only.
wrap400(async ({ user, body }) => {
    const gameId = body?.game_id;
    if (typeof gameId !== 'string' || gameId.length === 0) throw new Error('game_id is required');
    const row = await loadRow(gameId);
    const table = await serverTable();

    // ---- kernel section ----
    const loaded = table.load(row.state, row.roster);
    if (loaded < 0) throw new Error(`Game ${gameId} does not load (${loaded})`);
    const seat = table.seatOf(user.id);
    const needsBots = table.needsBots();
    const envelope = table.envelope(gameId, seat, row.version);
    // ---- end of the kernel section ----

    if (body?.type === 'bump') {
        if (typeof envelope === 'number') throw new Error(`Game ${gameId}: no envelope (${envelope})`);
        return { body: envelope, runBots: needsBots ? gameId : null };
    }
    if (seat < 0) throw new Error(`Player ${user.id} not in game ${gameId}`);
    throw new Error(`action type "${body?.type}" must be sent as a packed action request`);
}, packedAction);
