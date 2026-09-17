// client_read.ts - what the web client reads, the way the web client reads it.
//
// The browser reads every envelope and push through the kernel's client slot
// (sdk/ts/table/client_table.ts) and maps them onto today's game shape
// (src/state/snapshotToGame.ts). Tests that assert on what a player or a
// spectator is shown read through the same door, so they hold the product's
// reader rather than a second one.

import { clientTable } from '../../sdk/ts/table/client_table.ts';
import { pushToSequence } from '../../src/state/snapshotToGame.ts';

export { decodeEnvelope } from '../../src/state/snapshotToGame.ts';

/** Who sits where, as a test states it (the shape the realtime `r` extra had). */
export interface ReadRoster {
    id: string;
    name: string;
    players: { player_id: string; name: string; is_ai: boolean }[];
}

/**
 * A realtime push (as a server labels it `as2`: the sequence, or the whole as3
 * push) read with the seats named by `roster`, or null when it does not read
 * whole or does not seat the roster's players.
 */
export function readPush(bytes: Uint8Array, roster: ReadRoster, opts: { now?: () => number } = {}) {
    const table = clientTable();
    const identity = table.identityFromSeats(roster.id, roster.name,
        roster.players.map((p) => ({ id: p.player_id, name: p.name, isAi: p.is_ai })));
    if (!identity) return null;
    const read = table.readPush(bytes, { as3: false, identity });
    return read ? pushToSequence(read, { now: opts.now }) : null;
}
