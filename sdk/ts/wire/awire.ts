// Action wire ("awire" v1) — TS mirror of c/src/awire.h. The client
// builds ONE buffer per move and uses it for the guards-wasm gate, the
// optimistic apply, and the POST body; the server kernel applies the same
// bytes verbatim. Pure TS, no wasm imports.
import { Card } from "../../../server/api/core/types.ts";

export const AWIRE_KIND = {
    attack: 0,
    cover: 1,
    pass: 2,
    pickup: 3,
    good: 4,
} as const;
export type AwireKindName = keyof typeof AWIRE_KIND;

export const AWIRE_MAX_CARDS = 28;

// Mirrors wire.h / clientGuards wireCard: clamp into the representable
// space; the kernel re-clamps on decode (memory safety never depends on
// this side).
export function wireCard(c: Card): number {
    const suit = Math.min(3, Math.max(0, c.suit | 0));
    const value = Math.min(13, Math.max(1, c.value | 0));
    return suit * 13 + (value - 1);
}

export const WIRE_HIDDEN = 0xfe;
export const WIRE_NONE = 0xff;

export function cardFromWireByte(b: number): Card {
    if (b === WIRE_HIDDEN) return { suit: -1, value: -1 };
    const clamped = b > 51 ? 51 : b;
    return { suit: Math.floor(clamped / 13), value: (clamped % 13) + 1 };
}

export interface AwireMove {
    kind: AwireKindName;
    cards?: Card[];
    attack_cards?: Card[]; // cover only, positional with cards
}

// Move -> awire bytes. Throws on unencodable input (too many cards,
// mismatched cover pairs) — those are client bugs, not race conditions.
export function encodeAction(move: AwireMove): Uint8Array {
    const kind = AWIRE_KIND[move.kind];
    const cards = move.cards ?? [];
    if (kind === undefined) throw new Error(`awire: unknown kind ${move.kind}`);
    if (cards.length > AWIRE_MAX_CARDS) throw new Error(`awire: ${cards.length} cards exceeds ${AWIRE_MAX_CARDS}`);
    if ((move.kind === 'pickup' || move.kind === 'good') && cards.length > 0) {
        throw new Error(`awire: ${move.kind} carries no cards`);
    }
    const out = [kind, cards.length];
    for (const c of cards) out.push(wireCard(c));
    if (move.kind === 'cover') {
        const attacks = move.attack_cards ?? [];
        if (attacks.length !== cards.length) {
            throw new Error(`awire: cover pairs mismatched (${cards.length} covers, ${attacks.length} attacks)`);
        }
        for (const c of attacks) out.push(wireCard(c));
    }
    return new Uint8Array(out);
}

// awire bytes -> move, mirroring awire_decode's strictness (null on any
// malformed payload). Used by the legacy-fallback server path and tests.
export function decodeAction(buf: Uint8Array): AwireMove | null {
    if (buf.length < 2) return null;
    const kind = buf[0], n = buf[1];
    const name = (Object.keys(AWIRE_KIND) as AwireKindName[]).find(k => AWIRE_KIND[k] === kind);
    if (!name) return null;
    if (n > AWIRE_MAX_CARDS) return null;
    if ((name === 'pickup' || name === 'good') && n !== 0) return null;
    const expected = 2 + n * (name === 'cover' ? 2 : 1);
    if (buf.length !== expected) return null;
    const cards: Card[] = [];
    for (let i = 0; i < n; i++) cards.push(cardFromWireByte(buf[2 + i] > 51 ? 51 : buf[2 + i]));
    const move: AwireMove = { kind: name, cards };
    if (name === 'cover') {
        const attacks: Card[] = [];
        for (let i = 0; i < n; i++) attacks.push(cardFromWireByte(buf[2 + n + i] > 51 ? 51 : buf[2 + n + i]));
        move.attack_cards = attacks;
    }
    return move;
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
