// ask.ts - a test's request that is not a move, as the bytes a client sends.
//
// The suites state a lobby request the way they always have - { type: 'join',
// game_id, ... }, the old JSON body's names - because that reads well in a
// test. Nothing sends that object any more: this turns it into an ask
// (c/src/ask.h) through the same kernel writer the web uses
// (sdk/ts/table/client_table.ts askBody), so the bytes a test posts are the
// bytes a client posts. A field no ask carries (player_id on a start, seat,
// user_id: the spoofing suites add them) has nowhere to go and is dropped,
// which is the point those suites make.
import { clientTable, type Ask } from '../../sdk/ts/table/client_table.ts';
import * as V from '../../sdk/ts/gen/view_layout.bots.ts';
import { fixtureTable } from './table_fixture.ts';

// A Map, not an object: a hostile suite asks for the type '__proto__'.
const KIND = new Map<string, number>([
    ['join', V.ASK_JOIN], ['start', V.ASK_START], ['add-bot', V.ASK_ADD_BOT], ['exit', V.ASK_EXIT],
    ['continue', V.ASK_CONTINUE], ['rearrange-hand', V.ASK_REARRANGE_HAND],
    ['rearrange-players', V.ASK_REARRANGE_PLAYERS], ['update-name', V.ASK_UPDATE_NAME],
    ['set-rules', V.ASK_SET_RULES], ['bump', V.ASK_BUMP],
]);

const str = (v: unknown): string => (typeof v === 'string' ? v : '');

/** The ask a legacy-named request object states. Throws for a `type` that names no ask. */
export function askOf(body: Record<string, unknown>): Ask {
    const kind = KIND.get(str(body.type));
    if (kind === undefined) throw new Error(`askOf: "${String(body.type)}" names no ask`);
    return {
        kind, gid: str(body.game_id),
        bot: str(body.bot_id), player: str(body.player_id),
        idx: Array.isArray(body.card_indices) ? body.card_indices as number[] : [],
        order: Array.isArray(body.new_order) ? (body.new_order as string[]).map((id) => ({ id })) : [],
        title: str(body.new_name),
        passing: body.passing === true ? 1 : 0,
    };
}

/** The request body for that ask, written by the kernel. Throws when the kernel will not write it. */
export function askBody(body: Record<string, unknown>): Uint8Array {
    const bytes = clientTable().askBody(askOf(body));
    if (!bytes) throw new Error(`askBody: the kernel refused ${JSON.stringify(body)} (${JSON.stringify(clientTable().lastRefusal())})`);
    return bytes;
}

/**
 * What a request body asks, by the old request's name ('start', 'set-rules', ...),
 * or '' for bytes that are not an ask. Read through the server's decoder, for a
 * harness that records what a page sent.
 */
export function askKindName(body: Uint8Array): string {
    const ask = fixtureTable().askDecode(body);
    if (typeof ask === 'number') return '';
    for (const [name, kind] of KIND) if (kind === ask.kind) return name;
    return '';
}
