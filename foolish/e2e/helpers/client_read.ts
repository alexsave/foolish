// client_read.ts - what the web client reads, the way the web client reads it.
//
// The browser reads every envelope and push through the kernel's client slot
// (sdk/ts/table/client_table.ts) into TableView boards (src/state/view.ts), and
// names a push's events for its animation pipeline (src/state/pushSequence.ts).
// Tests that assert on what a player or a spectator is shown read through the
// same door, so they hold the product's reader rather than a second one:
// readPushSequence is exactly the web's.

import { clientTable, type TableView } from '../../sdk/ts/table/client_table.ts';
import { pushToSequence as viewSequence } from '../../src/state/pushSequence.ts';

/** An envelope as the web holds it: the board, or null when it does not read whole. */
export const readEnvelopeView = (bytes: Uint8Array): TableView | null => clientTable().adoptEnvelope(bytes);

/** Who sits where, as a test states it (the shape the realtime `r` extra had). */
export interface ReadRoster {
    id: string;
    name: string;
    players: { player_id: string; name: string; is_ai: boolean }[];
}

/**
 * A realtime push (as a server labels it `as2`: the sequence, or the whole as3
 * push) as the web's animation pipeline receives it: events and boards
 * (TableView), with the seats named by `roster`; null when it does not read whole
 * or does not seat the roster's players.
 */
export function readPushSequence(bytes: Uint8Array, roster: ReadRoster) {
    const read = readNamedPush(bytes, roster);
    return read ? viewSequence(read) : null;
}

function readNamedPush(bytes: Uint8Array, roster: ReadRoster) {
    const table = clientTable();
    const identity = table.identityFromSeats(roster.id, roster.name,
        roster.players.map((p) => ({ id: p.player_id, name: p.name, isAi: p.is_ai })));
    if (!identity) return null;
    return table.readPush(bytes, { as3: false, identity });
}
