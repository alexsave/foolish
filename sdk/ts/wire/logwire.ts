// Session log wire ("logwire" v1) — the packed, append-only byte form of a
// game session's log stream, stored as games.logs_packed (hex). The SOLE
// session-log store: the old JSONB game_logs table was dropped in migration
// 20260708120000 (docs/PACKED_WIRE_CUTOVER.md).
//
//   per record:
//     u48 LE created_at (epoch ms — the replay extras need per-move timing)
//     u8  log_type      (LOG_TYPE_FROM_INT order, mirrors LOG_* in game.h)
//     u8  seat          (player index, 0xFF = system)
//     u8  defender_idx  (0xFF = none)
//     u8  n_pairs
//     n_pairs x (u8 primary, u8 target)   wire cards; 0xFE hidden, 0xFF none
//
// No header: records concatenate, so commit_game appends a move's records
// with plain hex concatenation under the same version fence as the state
// blob (exactly-once by construction — a conflicted commit appends nothing).
// DRAW identities are already masked when the bytes are produced (the
// kernel's wasm_export_logs_masked, or the TS appendLogs convention for the
// JS paths), so the stored stream is safe for any reader — including bot
// belief imports, which must never see hidden draws (the no-cheating
// contract). Pure TS, no wasm imports.
import { GameLog, LOG_TYPE, LogType } from "../../../supabase/functions/_shared/core/types.ts";
import { Card } from "../../../supabase/functions/_shared/core/types.ts";
import { cardFromWireByte, WIRE_HIDDEN, WIRE_NONE, wireCard } from "./awire.ts";

const LOG_TYPE_FROM_INT: LogType[] = [
    LOG_TYPE.GAME_START, LOG_TYPE.ATTACK, LOG_TYPE.COVER, LOG_TYPE.PASS,
    LOG_TYPE.PICKUP, LOG_TYPE.GOOD, LOG_TYPE.DISCARD, LOG_TYPE.DEFENDER_CHANGE,
    LOG_TYPE.PLAYER_OUT, LOG_TYPE.DRAW,
];
const LOG_TYPE_TO_INT = new Map<string, number>(LOG_TYPE_FROM_INT.map((t, i) => [t, i]));

const SEAT_NONE = 0xff;

// Hidden cards ({suit:-1,value:-1}, appendLogs' convention) encode as the
// wire hidden byte; absent targets as none.
const wireLogCard = (c: Card | null | undefined): number => {
    if (c === null || c === undefined) return WIRE_NONE;
    if (c.suit < 0 || c.value < 1) return WIRE_HIDDEN;
    return wireCard(c);
};

function pushTs(out: number[], ms: number): void {
    // u48 covers epoch ms until the year 10889.
    let v = ms;
    for (let i = 0; i < 6; i++) { out.push(v % 256); v = Math.floor(v / 256); }
}

function readTs(buf: Uint8Array, q: number): number {
    let v = 0;
    for (let i = 5; i >= 0; i--) v = v * 256 + buf[q + i];
    return v;
}

// JS GameLog[] (the appendLogs output — DRAW identities already hidden) ->
// logwire records. `seatOf` maps player ids to seats (session rosters are
// seat-stable: rearrange only exists in WAITING games).
export function encodeLogs(logs: GameLog[], seatOf: (pid: string | null) => number): Uint8Array {
    const out: number[] = [];
    for (const l of logs) {
        const t = LOG_TYPE_TO_INT.get(l.log_type);
        if (t === undefined) throw new Error(`logwire: unknown log type ${l.log_type}`);
        pushTs(out, new Date(l.created_at).getTime());
        out.push(t);
        const seat = l.player_id !== null && l.player_id !== undefined ? seatOf(l.player_id) : -1;
        out.push(seat >= 0 ? seat : SEAT_NONE);
        out.push(l.defender_index !== null && l.defender_index !== undefined ? l.defender_index & 0xff : SEAT_NONE);
        const pairs = l.card_pairs ?? [];
        if (pairs.length > 255) throw new Error(`logwire: ${pairs.length} pairs exceeds the wire cap`);
        out.push(pairs.length);
        for (const p of pairs) {
            out.push(wireLogCard(p.primary));
            out.push(wireLogCard(p.target));
        }
    }
    return new Uint8Array(out);
}

// The kernel's wasm_export_logs(_masked) buffer (u16 count + timestamp-less
// records) -> logwire records, stamping one clock value across the move (the
// old path stamped each move's rows within the same millisecond anyway).
// Byte splice only — no JS log objects.
export function logsFromKernelExport(kernelBytes: Uint8Array, nowMs: number): Uint8Array {
    const n = kernelBytes[0] | (kernelBytes[1] << 8);
    const out: number[] = [];
    let q = 2;
    for (let i = 0; i < n; i++) {
        if (q + 4 > kernelBytes.length) throw new RangeError('logwire: truncated kernel log export');
        pushTs(out, nowMs);
        out.push(kernelBytes[q], kernelBytes[q + 1], kernelBytes[q + 2]);
        const nPairs = kernelBytes[q + 3];
        out.push(nPairs);
        q += 4;
        if (q + nPairs * 2 > kernelBytes.length) throw new RangeError('logwire: truncated kernel log export');
        for (let j = 0; j < nPairs * 2; j++) out.push(kernelBytes[q++]);
    }
    return new Uint8Array(out);
}

// A round closes exactly when the defender takes the table (PICKUP) or the
// covered table is trashed (DISCARD) — the two log records that rotate
// first_attacker/defender and refill hands. The server's round-boundary guard
// (docs/WEB_RACE_BUG_HANDOFF.md) stamps games.round_epoch with the new version
// on any commit whose move produced one of these, so it scans this move's
// packed records for them. Byte scan only — no JS log objects, no wasm; safe
// on the DRAW-masked stored stream. Same record layout as decodeLogs.
const LOG_PICKUP_INT = LOG_TYPE_TO_INT.get(LOG_TYPE.PICKUP)!;
const LOG_DISCARD_INT = LOG_TYPE_TO_INT.get(LOG_TYPE.DISCARD)!;

export function logwireClosesRound(buf: Uint8Array): boolean {
    let q = 0;
    while (q < buf.length) {
        if (q + 10 > buf.length) return false; // truncated tail — nothing more to read
        const type = buf[q + 6];
        if (type === LOG_PICKUP_INT || type === LOG_DISCARD_INT) return true;
        const nPairs = buf[q + 9];
        q += 10 + nPairs * 2;
    }
    return false;
}

// Same scan, straight on the BARE-hex string commit_game stores — no
// Uint8Array allocation and no codec import on the per-commit hot path (the
// only caller, commitGame, holds the logs as hex already). Each byte is two
// hex chars, so the byte offsets above double: type at chars [12,14), nPairs at
// [18,20), record stride 10+2·nPairs bytes = 20+4·nPairs chars. Two hex nibbles
// parse without allocating via charCode math (no slice/substr per record).
const hx = (c: number): number => (c <= 57 ? c - 48 : (c <= 70 ? c - 55 : c - 87)); // '0'-'9' | 'A'-'F' | 'a'-'f'
const byteAt = (h: string, charIdx: number): number => (hx(h.charCodeAt(charIdx)) << 4) | hx(h.charCodeAt(charIdx + 1));

export function logwireHexClosesRound(hex: string): boolean {
    const start = hex.startsWith('\\x') ? 2 : 0; // hexToBytes tolerated both; match it
    let q = start;
    while (q + 20 <= hex.length) { // a full 10-byte record header = 20 hex chars
        const type = byteAt(hex, q + 12);
        if (type === LOG_PICKUP_INT || type === LOG_DISCARD_INT) return true;
        const nPairs = byteAt(hex, q + 18);
        q += 20 + nPairs * 4;
    }
    return false;
}

// logwire bytes -> GameLog[] for the cold-path consumers that still want JS
// objects (the end-of-game replay encode). Ids are synthesized (the old ones
// only served insert idempotence); order is byte order (a total order — the
// records were appended under the commit version fence).
export function decodeLogs(
    buf: Uint8Array, gameId: string, players: { player_id: string }[],
): GameLog[] {
    const logs: GameLog[] = [];
    let q = 0;
    while (q < buf.length) {
        if (q + 10 > buf.length) throw new RangeError(`logwire: truncated record at ${q}/${buf.length}`);
        const ts = readTs(buf, q); q += 6;
        const type = LOG_TYPE_FROM_INT[buf[q++]];
        if (type === undefined) throw new RangeError('logwire: unknown log type byte');
        const seat = buf[q++];
        const def = buf[q++];
        const nPairs = buf[q++];
        if (q + nPairs * 2 > buf.length) throw new RangeError(`logwire: truncated pairs at ${q}/${buf.length}`);
        const pairs: { primary: Card; target: Card | null }[] = [];
        for (let j = 0; j < nPairs; j++) {
            const pw = buf[q++];
            const tw = buf[q++];
            pairs.push({
                primary: pw === WIRE_HIDDEN ? { suit: -1, value: -1 } : cardFromWireByte(pw),
                target: tw === WIRE_NONE ? null : cardFromWireByte(tw),
            });
        }
        logs.push({
            id: crypto.randomUUID(),
            created_at: new Date(ts).toISOString(),
            game_id: gameId,
            log_type: type,
            player_id: seat !== SEAT_NONE ? (players[seat]?.player_id ?? null) : null,
            card_pairs: pairs,
            defender_index: def !== SEAT_NONE ? def : null,
        } as GameLog);
    }
    return logs;
}
