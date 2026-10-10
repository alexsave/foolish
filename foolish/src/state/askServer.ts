// askServer.ts - every request to the server that is not a move.
//
// A move goes out as the kernel's action request (ServerContext
// invokePackedAction). Everything else - the lobby edits and the bot nudge - is
// an ask (c/src/ask.h): this file hands the kernel the ask, posts the bytes it
// wrote, and hands the kernel the bytes that came back. It knows no layout in
// either direction, and no JSON crosses: the answer is an envelope
// (client_table.h), a refusal is the kernel's AskRefusal.

import supabase from '../backend/Connector';
import { clientTable, type Ask, type AskRefusal } from '@sdk/ts/table/client_table.ts';
import { ASK_BUMP } from '@sdk/ts/gen/view_layout.bots.ts';
import type { TableView } from './view';

/** The server answered an ask, or a create, with a refusal: `why` an ASK_WHY_*, `code` and `detail` the kernel's own. */
export class AskRefused extends Error {
    constructor(readonly endpoint: string, readonly status: number, readonly refusal: AskRefusal | null) {
        super(refusal
            ? `${endpoint} refused (HTTP ${status}): why ${refusal.why}, code ${refusal.code}, detail ${refusal.detail}`
            : `${endpoint} failed (HTTP ${status})`);
        this.name = 'AskRefused';
    }
}

/** A function response's bytes: functions-js hands an octet-stream back as a Blob. */
async function bytesOf(data: unknown): Promise<Uint8Array | null> {
    if (typeof Blob !== 'undefined' && data instanceof Blob) return new Uint8Array(await data.arrayBuffer());
    if (data instanceof ArrayBuffer) return new Uint8Array(data);
    return null;
}

/** What a failed invoke says: the refusal in its response, read through the kernel. */
async function refusedBy(endpoint: string, error: unknown): Promise<AskRefused> {
    // functions-js puts the Response of a non-2xx answer on error.context.
    const res = (error as { context?: unknown })?.context;
    if (typeof Response === 'undefined' || !(res instanceof Response)) return new AskRefused(endpoint, 0, null);
    let refusal: AskRefusal | null = null;
    try {
        refusal = clientTable().readRefusal(new Uint8Array(await res.arrayBuffer()));
    } catch { /* a body that is not a refusal: the status is all there is */ }
    return new AskRefused(endpoint, res.status, refusal);
}

/**
 * POSTs to an edge function and reads the envelope it answers with. `body`
 * null: a request with nothing to say (create). functions-js sends a Blob as
 * application/octet-stream, which is the only body the server reads.
 */
async function post(endpoint: string, body: Uint8Array | null): Promise<TableView> {
    const { data, error } = await supabase.functions.invoke(endpoint, body
        ? { body: new Blob([body as Uint8Array<ArrayBuffer>]) }
        : { method: 'POST' });
    if (error) throw await refusedBy(endpoint, error);
    const bytes = await bytesOf(data);
    const view = bytes ? clientTable().adoptEnvelope(bytes) : null;
    if (!view || !view.gameId) throw new Error(`Invalid response from ${endpoint}: not an envelope`);
    return view;
}

/** Sends an ask to `meta`; the caller's board as the server now has it. */
export function sendAsk(ask: Ask): Promise<TableView> {
    const body = clientTable().askBody(ask);
    if (!body) return Promise.reject(new Error(`ask ${ask.kind} for "${ask.gid}" is not one the kernel writes`));
    return post('meta', body);
}

/**
 * The bot-loop nudge (ASK_BUMP): sent and forgotten. The server answers with the
 * caller's envelope, which nobody needs - the board arrives by its own channel -
 * so the answer is not read; only a refusal or a failed request rejects.
 */
export async function sendBump(gameId: string): Promise<void> {
    const body = clientTable().askBody({ kind: ASK_BUMP, gid: gameId });
    if (!body) throw new Error(`bump for "${gameId}" is not an ask the kernel writes`);
    const { error } = await supabase.functions.invoke('meta', { body: new Blob([body as Uint8Array<ArrayBuffer>]) });
    if (error) throw await refusedBy('meta', error);
}

/** Creates a game: the creator's board. The request carries nothing but the caller's token. */
export function sendCreate(): Promise<TableView> {
    return post('create', null);
}
