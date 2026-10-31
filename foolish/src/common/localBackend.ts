// localBackend.ts - backend.ts's answers for a game played in this tab (the
// promo codes, src/common/promo.ts): the kernel's local table
// (c/src/local_table.h) stands where c_base would, so nothing leaves the page.
//
// The kernel is the server here, and decides what a server decides: the deal,
// whether a move applies, the bot's move and when it is due. What stays in this
// file is a server's transport: hand a request in, hand the human's cast to
// the stream's listeners, and wake the bot when the kernel says.

import { NetConst as N, type NetAction } from '@sdk/ts/net/net_client.ts';
import { clientTable } from '@sdk/ts/table/client_table.ts';
import { localTable } from '@sdk/ts/table/local_table.ts';

export interface LocalBackend {
    view(gameId: string): Uint8Array | null;
    action(gameId: string, wire: Uint8Array, intent: number): NetAction;
    /** The kernel's TABLE_* result of an ask, negative for a refusal. */
    meta(body: Uint8Array): number;
    hold(what: number, gameId: string): () => void;
    /** The finished game's replay, or null before the end. */
    replay(gameId: string): { moves: Uint8Array; extras: Uint8Array } | null;
    stop(): void;
}

/** A local table `gameId`, its human named `you`, whose casts go to `cast`. */
export function localBackend(gameId: string, you: string, cast: (gameId: string, bytes: Uint8Array) => void): LocalBackend {
    const table = localTable();
    let opened = false, dealt = false, stopped = false;
    let timer: ReturnType<typeof setTimeout> | null = null;

    // The cast the last operation left, out to the page as a socket delivers it
    // (after the call that caused it has answered, in commit order), and the bot
    // woken when the kernel says it is due.
    const flush = () => {
        const bytes = table.cast();
        if (bytes) setTimeout(() => { if (!stopped) cast(gameId, bytes); }, 0);
        if (timer) clearTimeout(timer);
        timer = null;
        const ms = stopped ? -1 : table.botWaitMs();
        if (ms >= 0) timer = setTimeout(() => { timer = null; table.bot(); flush(); }, ms);
    };

    return {
        view(gid) {
            if (gid !== gameId) return null;
            if (opened) return table.view();
            opened = true;
            return table.open(gameId, crypto.getRandomValues(new Uint8Array(32)), you);
        },
        action(gid, wire, intent) {
            const ct = clientTable();
            const body = table.action(ct.actionRequest(gid, wire, intent < 0 ? undefined : intent));
            flush();
            const r = ct.readActionResponse(body);
            if (!r) throw new Error('local table: unreadable action response');
            return { status: r.status, rejectCode: r.rejectCode, version: r.version };
        },
        meta(body) {
            const rc = table.ask(body);
            flush();
            return rc;
        },
        // The page holding the game's stream is the human sitting down at it:
        // the table deals, and the deal is the first cast the stream carries.
        hold(what, gid) {
            if (what === N.NW_SUB_GAME && gid === gameId && opened && !dealt) {
                dealt = true;
                setTimeout(() => { if (!stopped) { table.start(); flush(); } }, 0);
            }
            return () => {};
        },
        replay(gid) {
            if (gid !== gameId || !opened) return null;
            try { return table.replay(); } catch { return null; }
        },
        stop() {
            stopped = true;
            if (timer) clearTimeout(timer);
            timer = null;
        },
    };
}
