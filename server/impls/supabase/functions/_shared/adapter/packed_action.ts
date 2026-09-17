// The `action` endpoint's move path (docs/C_GAME_SHAPE_MIGRATION.md 2.3): a
// human move as bytes end to end.
//
// The request body is the kernel's action request ([fmt | gid_len | game id |
// (intent version) | action wire]) and the response is the kernel's action
// response ([fmt | status | code | u32 version]); both layouts are C's
// (table_request_decode, table_action_response). The move itself - the seat of
// the auth id, the finished-game and stale-round guards, the rules, the finish,
// the masking, the events - is table_act and the commit products. This module
// is the CAS loop's caller: it says which results commit nothing, and that a
// refusal read off this isolate's cache is re-checked against the database.

import * as L from '@sdk/ts/gen/game_layout.bots.ts';
import { serverTable } from '@sdk/ts/table/server_table.ts';
import { runTableOp, TableRefusal } from './table_io.ts';

export interface ActionOutcome {
    /** The response body. */
    body: Uint8Array;
    gameId: string;
    /** The committed row has a bot to drive. */
    needsBots: boolean;
}

/** A request the kernel could not parse. */
export class MalformedActionRequest extends Error {
    constructor() { super('malformed action request'); this.name = 'MalformedActionRequest'; }
}

// A move that commits nothing: the game was already over, the move was composed
// before the current round began, or the rules refused it.
const noCommit = (rc: number) => rc === L.TABLE_MOOT || rc === L.TABLE_STALE_ROUND || rc === L.TABLE_REJECTED;
// A refusal is only authoritative against fresh state: an apply from a stale
// cache self-corrects through the CAS conflict, but a refusal never reaches it.
const freshOnly = (rc: number) => rc === L.TABLE_STALE_ROUND || rc === L.TABLE_REJECTED;

export async function executePackedAction(requestBody: Uint8Array, userId: string, reqId = 'action'): Promise<ActionOutcome> {
    const table = await serverTable();
    const request = table.requestDecode(requestBody);
    if (typeof request === 'number') throw new MalformedActionRequest();
    const { gameId, wire, intent } = request;

    let out;
    try {
        out = await runTableOp({
            gameId, reqId, viewerId: userId, noCommit, freshOnly,
            run: ({ table: t, row }) => t.act(userId, wire, intent, row.roundEpoch),
        });
    } catch (e) {
        if (e instanceof TableRefusal && e.code === L.TABLE_E_WIRE) throw new MalformedActionRequest();
        throw e;
    }
    // The response is written in its own tiny kernel call: the section that
    // produced `out` ended at the commit's await.
    return { body: table.actionResponse(out.result, out.reject, out.version), gameId, needsBots: out.committed && out.needsBots };
}
