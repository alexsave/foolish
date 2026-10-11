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

// An ask's fields are typed: an id is text, a list is a list, the rule is a
// yes or a no. A value of another type is one the hostile suites used to put in
// a JSON body - `bot_id: { $ne: null }`, `passing: 0` - and no ask can carry it,
// so stating one is refused HERE rather than read as the field being absent
// (an exit with no bot named is the caller leaving, which is not what was asked).
const absent = (v: unknown): boolean => v === undefined;
function text(body: Record<string, unknown>, field: string): string {
    const v = body[field];
    if (absent(v)) return '';
    if (typeof v !== 'string') throw new Error(`askOf: ${field} is not text, and an ask carries nothing else there`);
    return v;
}
function list(body: Record<string, unknown>, field: string): unknown[] {
    const v = body[field];
    if (absent(v)) return [];
    if (!Array.isArray(v)) throw new Error(`askOf: ${field} is not a list, and an ask carries nothing else there`);
    return v;
}

/** The ask a legacy-named request object states. Throws for a `type` that names no ask, and for a field no ask can carry. */
export function askOf(body: Record<string, unknown>): Ask {
    const kind = typeof body.type === 'string' ? KIND.get(body.type) : undefined;
    if (kind === undefined) throw new Error(`askOf: "${String(body.type)}" names no ask`);
    if (kind === V.ASK_SET_RULES && typeof body.passing !== 'boolean') {
        throw new Error('askOf: passing is not a yes or a no, and an ask carries nothing else there');
    }
    return {
        kind, gid: text(body, 'game_id'),
        bot: text(body, 'bot_id'), player: text(body, 'player_id'),
        idx: list(body, 'card_indices') as number[],
        order: (list(body, 'new_order') as string[]).map((id) => ({ id })),
        title: text(body, 'new_name'),
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
