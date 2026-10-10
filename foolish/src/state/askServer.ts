// askServer.ts - every request to the server that is not a move is an ask
// (c/src/ask.h): the kernel writes it, this posts the bytes, and the kernel
// reads the envelope or the AskRefusal that comes back.

import supabase from '../backend/Connector';
import { clientTable, type Ask } from '@sdk/ts/table/client_table.ts';
import { ASK_BUMP } from '@sdk/ts/gen/view_layout.bots.ts';
import type { TableView } from './view';

/**
 * POSTs bytes to an edge function (`body` null: a request with nothing to say).
 * functions-js sends a Blob as application/octet-stream, the only body the
 * server reads, and hands an octet-stream answer back as a Blob.
 */
export async function invokeBytes(endpoint: string, body: Uint8Array | null): Promise<{ bytes: Uint8Array | null; error: unknown }> {
    const { data, error } = await supabase.functions.invoke(endpoint, body
        ? { body: new Blob([body as Uint8Array<ArrayBuffer>]) }
        : { method: 'POST' });
    const bytes = error ? null
        : typeof Blob !== 'undefined' && data instanceof Blob ? new Uint8Array(await data.arrayBuffer())
        : data instanceof ArrayBuffer ? new Uint8Array(data) : null;
    return { bytes, error };
}

/** What a failed invoke says: the kernel's refusal in its response, which functions-js puts on error.context. */
async function refused(endpoint: string, error: unknown): Promise<Error> {
    const res = (error as { context?: unknown })?.context;
    if (typeof Response === 'undefined' || !(res instanceof Response)) return new Error(`${endpoint} failed`);
    const r = await res.arrayBuffer().then((b) => clientTable().readRefusal(new Uint8Array(b)), () => null);
    return new Error(r
        ? `${endpoint} refused (HTTP ${res.status}): why ${r.why}, code ${r.code}, detail ${r.detail}`
        : `${endpoint} failed (HTTP ${res.status})`);
}

function bodyOf(ask: Ask): Uint8Array {
    const body = clientTable().askBody(ask);
    if (!body) throw new Error(`ask ${ask.kind} for "${ask.gid}" is not one the kernel writes`);
    return body;
}

/** The caller's board as the server now has it: the envelope every ask and a create answer with. */
async function post(endpoint: string, body: Uint8Array | null): Promise<TableView> {
    const { bytes, error } = await invokeBytes(endpoint, body);
    if (error) throw await refused(endpoint, error);
    const view = bytes ? clientTable().adoptEnvelope(bytes) : null;
    if (!view || !view.gameId) throw new Error(`Invalid response from ${endpoint}: not an envelope`);
    return view;
}

export async function sendAsk(ask: Ask): Promise<TableView> {
    return post('meta', bodyOf(ask));
}

/** The bot-loop nudge: the answering envelope is not adopted, because the board arrives by its own channel. */
export async function sendBump(gameId: string): Promise<void> {
    const { error } = await invokeBytes('meta', bodyOf({ kind: ASK_BUMP, gid: gameId }));
    if (error) throw await refused('meta', error);
}

/** Creates a game: the request carries nothing but the caller's token. */
export function sendCreate(): Promise<TableView> {
    return post('create', null);
}
