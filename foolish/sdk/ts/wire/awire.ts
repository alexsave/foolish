// The action wire ("awire" v1, c/src/awire.h) as TypeScript reaches it. The
// client builds ONE buffer per move and uses it for the kernel's gate and
// optimistic board (c/src/client_table.h), and the POST body; the server kernel
// applies the same bytes verbatim.
//
// NO BYTE OF IT IS WRITTEN HERE. This file used to be a second implementation
// of awire.c - its own frame, its own card byte, its own copies of the kinds -
// and the two could only be held together by tests. A move is now a plain
// object that goes into the kernel through the generated ClientMove writer
// (sdk/ts/gen/view_layout.bots.ts), and awire.c's one encoder writes the wire.
// What is left here is the names a caller says a move with.

import * as V from '../gen/view_layout.bots.ts';
import { clientTable } from '../table/client_table.ts';

/** A card of a move (suit 0..3, value 1..13). */
export interface Card { readonly suit: number; readonly value: number }

/** A move's kinds by name, numbered by the kernel (awire.h AWIRE_*). */
export const AWIRE_KIND = {
    attack: V.AWIRE_ATTACK,
    cover: V.AWIRE_COVER,
    pass: V.AWIRE_PASS,
    pickup: V.AWIRE_PICKUP,
    good: V.AWIRE_GOOD,
} as const;
export type AwireKindName = keyof typeof AWIRE_KIND;

export const AWIRE_MAX_CARDS = V.AWIRE_MAX_CARDS;

export interface AwireMove {
    kind: AwireKindName;
    cards?: Card[];
    attack_cards?: Card[]; // cover only, positional with cards
}

// Move -> awire bytes, written by the kernel (client_move_wire). Throws on a
// move the wire cannot carry (too many cards, cards on a pickup, mismatched
// cover pairs, a card that is not a card) - those are client bugs, not race
// conditions. Needs the kernel warm, as every board a move is made on already
// does (ensureBotsAsync).
export function encodeAction(move: AwireMove): Uint8Array {
    const kind = AWIRE_KIND[move.kind];
    if (kind === undefined) throw new Error(`awire: unknown kind ${move.kind}`);
    return clientTable().moveWire({ kind, cards: move.cards ?? [], attackCards: move.attack_cards ?? [] });
}

// ---------------------------------------------------------------------------
// HTTP envelopes (the `action` edge function's binary request/response)
// ---------------------------------------------------------------------------

// Request envelope formats. v1 = [fmt | gid_len | gid | wire]. v2 adds the
// client-intent round guard: [fmt | gid_len | gid | u32 intent_version | wire]
// — the games.version the client composed this move against. The server's
// round-boundary guard (packed_action.ts) rejects a move whose intent_version
// predates the current round (REJECT_STALE_ROUND), which is the stale-intent
// bug from docs/WEB_RACE_BUG_HANDOFF.md. v1 requests (old clients mid-rollout)
// carry no intent_version and are simply not guarded — today's behavior.
export const ACTION_REQ_FORMAT_V1 = 1;
export const ACTION_REQ_FORMAT = 2;
export const ACTION_RESP_FORMAT = 1;

// Response status byte.
export const ACTION_STATUS = { APPLIED: 0, REJECTED: 1, MOOT: 2 } as const;

// Server-edge reject codes live ABOVE the kernel's ENGINE_REJECT_* space
// (0..21 in c/src/game.h) so a client can tell a rules rejection from an
// edge-policy one by the code alone. REJECT_STALE_ROUND is not a kernel
// verdict — the move is kernel-legal against the CURRENT state; it is refused
// because a round closed after the client composed it (round-boundary rule,
// docs/IMESSAGE_GAME_DESIGN.md §7.4-§7.5).
export const REJECT_STALE_ROUND = 100;

export function encodeActionRequest(gameId: string, wire: Uint8Array, intentVersion?: number): Uint8Array {
    const gid = new TextEncoder().encode(gameId);
    if (gid.length > 255) throw new Error('awire: game id too long');
    if (intentVersion === undefined) {
        // Legacy v1 envelope — no intent guard.
        const out = new Uint8Array(2 + gid.length + wire.length);
        out[0] = ACTION_REQ_FORMAT_V1;
        out[1] = gid.length;
        out.set(gid, 2);
        out.set(wire, 2 + gid.length);
        return out;
    }
    const iv = intentVersion >>> 0;
    const out = new Uint8Array(2 + gid.length + 4 + wire.length);
    out[0] = ACTION_REQ_FORMAT;
    out[1] = gid.length;
    out.set(gid, 2);
    let p = 2 + gid.length;
    out[p++] = iv & 0xff;
    out[p++] = (iv >> 8) & 0xff;
    out[p++] = (iv >> 16) & 0xff;
    out[p++] = (iv >> 24) & 0xff;
    out.set(wire, p);
    return out;
}

export function decodeActionRequest(buf: Uint8Array): { gameId: string; wire: Uint8Array; intentVersion?: number } | null {
    if (buf.length < 2) return null;
    const fmt = buf[0];
    const gidLen = buf[1];
    if (fmt === ACTION_REQ_FORMAT_V1) {
        if (buf.length < 2 + gidLen + 2) return null; // wire is at least 2 bytes
        const gameId = new TextDecoder().decode(buf.subarray(2, 2 + gidLen));
        return { gameId, wire: buf.subarray(2 + gidLen) };
    }
    if (fmt === ACTION_REQ_FORMAT) {
        if (buf.length < 2 + gidLen + 4 + 2) return null; // u32 intent + 2-byte wire
        const gameId = new TextDecoder().decode(buf.subarray(2, 2 + gidLen));
        const p = 2 + gidLen;
        const intentVersion = (buf[p] | (buf[p + 1] << 8) | (buf[p + 2] << 16) | (buf[p + 3] << 24)) >>> 0;
        return { gameId, wire: buf.subarray(p + 4), intentVersion };
    }
    return null;
}

export function encodeActionResponse(status: number, rejectCode: number, version: number): Uint8Array {
    const out: number[] = [ACTION_RESP_FORMAT, status & 0xff, rejectCode & 0xff];
    out.push(version & 0xff, (version >> 8) & 0xff, (version >> 16) & 0xff, (version >> 24) & 0xff);
    return new Uint8Array(out);
}

export function decodeActionResponse(buf: Uint8Array): { status: number; rejectCode: number; version: number } | null {
    if (buf.length < 7 || buf[0] !== ACTION_RESP_FORMAT) return null;
    return {
        status: buf[1],
        rejectCode: buf[2],
        version: (buf[3] | (buf[4] << 8) | (buf[5] << 16) | (buf[6] << 24)) >>> 0,
    };
}
