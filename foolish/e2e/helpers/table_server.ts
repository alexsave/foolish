// table_server.ts - drive the REAL server modules on kernel-owned rows, below
// HTTP (docs/C_GAME_SHAPE_MIGRATION.md Phase 4b).
//
// The suites that exercise the server's CAS loop, commit and broadcast call the
// same functions the edge entry points call - handleMetaAction (meta),
// executePackedAction (action), lockedBotLoop (the bot loop) - with the caller's
// auth id, and read the result back through the kernel (table_play.ts). A suite
// that is about the HTTP shell itself (auth, the JSON body, the response) goes
// through e2e/helpers/edge.ts instead.

import { fixture, READY, IDLE, type FixtureSeat } from './table_fixture.ts';
import { seedTable } from './table_db.ts';
import { legalMoves, mustReadTable, type PlayMove, type TableState } from './table_play.ts';
import { handleMetaAction } from '../../server/impls/supabase/functions/_shared/adapter/meta_actions.ts';
import { executePackedAction } from '../../server/impls/supabase/functions/_shared/adapter/packed_action.ts';
import { lockedBotLoop } from '../../server/impls/supabase/functions/_shared/adapter/bot_actions.ts';
import type { HandlerResult } from '../../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { ACTION_STATUS, decodeActionResponse, encodeActionRequest } from '../../sdk/ts/wire/awire.ts';
import * as L from '../../sdk/ts/gen/game_layout.bots.ts';

export interface LobbySeat extends FixtureSeat {
    /** Ready in the lobby (default true, as the old seedGame seeded every seat). */
    ready?: boolean;
}

/**
 * A kernel-owned lobby row: the seats in order, each READY unless `ready: false`
 * (bots are always READY). The humans' auth.users rows and the bots' bots rows
 * are inserted by seedTable.
 */
export async function seedLobby(gameId: string, seats: LobbySeat[], title = gameId): Promise<void> {
    let b = fixture().title(title).seats(seats.map(({ id, name, brain }) => ({ id, name, brain })));
    seats.forEach((s, i) => { b = b.seatStatus(i, s.brain || s.ready !== false ? READY : IDLE); });
    await seedTable(gameId, b.build());
}

/** One `meta` request by `userId`, through the real handler. */
export function runMeta(gameId: string, userId: string, body: Record<string, unknown>, userName = 'U'): Promise<HandlerResult> {
    return handleMetaAction({
        user: { id: userId, user_metadata: { username: userName } } as never,
        userName, body: { ...body, game_id: gameId }, reqId: 'e2e-meta',
    });
}

export interface ActionResult {
    /** ACTION_STATUS.* */
    status: number;
    rejectCode: number;
    version: number;
    needsBots: boolean;
}

/** One packed move by `userId`, through the real move path. */
export async function runAction(gameId: string, userId: string, move: PlayMove | Uint8Array, intentVersion?: number): Promise<ActionResult> {
    const wire = move instanceof Uint8Array ? move : move.wire;
    const out = await executePackedAction(encodeActionRequest(gameId, wire, intentVersion), userId, 'e2e-action');
    const r = decodeActionResponse(out.body);
    if (!r) throw new Error('runAction: the response did not decode');
    return { ...r, needsBots: out.needsBots };
}

/** One bot-loop segment for the game, as the scheduler runs it. */
export const driveBots = (gameId: string): Promise<void> => lockedBotLoop(gameId);

export interface PlayOptions {
    /** Picks one of the legal moves (default: the first). */
    pick?: (moves: PlayMove[], t: TableState) => PlayMove;
    maxSteps?: number;
}

/**
 * Plays a dealt game to its end: humans move through the move path (a stale pick
 * under the CAS is tolerated, as a client's would be), and whenever the kernel
 * says bots have work the bot loop drives them. Returns the final table.
 */
export async function playToEnd(gameId: string, opts: PlayOptions = {}): Promise<TableState> {
    const pick = opts.pick ?? ((m) => m[0]);
    for (let step = 0; step < (opts.maxSteps ?? 2000); step++) {
        const t = await mustReadTable(gameId);
        if (t.status !== L.GAME_STATUS_PLAYING) return t;
        if (t.needsBotsColumn) {
            await driveBots(gameId);
            const after = await mustReadTable(gameId);
            if (after.version !== t.version) continue;
        }
        const moves = legalMoves(t, (s) => !s.brain);
        if (moves.length === 0) {
            if (t.needsBotsColumn) continue;
            throw new Error(`playToEnd: game ${gameId} has no human move and no bot work at version ${t.version}`);
        }
        const mv = pick(moves, t);
        const r = await runAction(gameId, mv.playerId, mv);
        if (r.status === ACTION_STATUS.MOOT) return mustReadTable(gameId);
    }
    throw new Error(`playToEnd: game ${gameId} did not end`);
}
