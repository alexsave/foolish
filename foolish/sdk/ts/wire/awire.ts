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
// The `action` endpoint's bodies (c/src/table.h: the request and the response)
// ---------------------------------------------------------------------------
//
// The server reads the request and writes the response in the kernel
// (table_request_decode, table_action_response). The client's halves are the
// kernel's too (table_request_encode, table_response_decode), reached through
// the client slot, so both ends of each body are one C file and the framing is
// written down once, there.

/** The response's status byte, numbered by the kernel (table.h TABLE_STATUS_*). */
export const ACTION_STATUS = {
    APPLIED: V.TABLE_STATUS_APPLIED,
    REJECTED: V.TABLE_STATUS_REJECTED,
    MOOT: V.TABLE_STATUS_MOOT,
} as const;

// Server-edge reject codes live ABOVE the kernel's ENGINE_REJECT_* space (c/src/game.h)
// so a client can tell a rules rejection from an edge-policy one by the code
// alone. REJECT_STALE_ROUND is not a kernel verdict on the move - the move is
// kernel-legal against the CURRENT state; it is refused because a round closed
// after the client composed it (round-boundary rule,
// docs/IMESSAGE_GAME_DESIGN.md §7.4-§7.5). The number is table.h's.
export const REJECT_STALE_ROUND = V.TABLE_REJECT_STALE_ROUND;

// The request body for a move's wire. `intentVersion` is the games.version the
// client composed the move against: the server's round-boundary guard rejects a
// move whose intent predates the current round (REJECT_STALE_ROUND), which is
// the stale-intent bug from docs/WEB_RACE_BUG_HANDOFF.md. Without one the body
// is the v1 form, which carries no intent and is simply not guarded.
export function encodeActionRequest(gameId: string, wire: Uint8Array, intentVersion?: number): Uint8Array {
    return clientTable().actionRequest(gameId, wire, intentVersion);
}

export function decodeActionResponse(buf: Uint8Array): { status: number; rejectCode: number; version: number } | null {
    return clientTable().readActionResponse(buf);
}
