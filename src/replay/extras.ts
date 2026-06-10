/* =============================================================================
 * Replay extras: player names + move timestamps (optional URL section)
 * =============================================================================
 * The move integer (encode.ts/decode.ts) is deliberately untouched — extras
 * are a SEPARATE blob appended to the URL code after a dash:
 *
 *     WWW.FOOLISH.CARDS/<base32 moves>                 (moves only)
 *     WWW.FOOLISH.CARDS/<base32 moves>-<base32 extras> (full)
 *
 * '-' is in the QR alphanumeric charset and not in the base32 alphabet, so
 * the full form still QR-encodes densely, and the moves-only code is always
 * just the prefix before the dash — store the full form, cut to get the
 * small one.
 *
 * Extras blob layout (version 1):
 *   byte 0          version (1)
 *   byte 1          flags: bit0 = names section, bit1 = times section
 *   names section   playerCount × (UTF-8 bytes + 0x00). playerCount comes
 *                   from the decoded moves payload.
 *   times section   5 bytes: game-start unix time, seconds, big-endian
 *                   then 1 byte per information-bearing move (ATTACK / COVER /
 *                   PASS / PICKUP / GOOD, in move order): the gap since the
 *                   previous move, log-quantized (see below).
 *
 * TIME SCALING. Move gaps span milliseconds (bot flurries) to weeks
 * (correspondence games), so a linear fixed-width field is hopeless and
 * varints waste bytes on the long tail. Instead each gap is one byte on a
 * μ-law-style logarithmic scale:
 *
 *     gap(v) = A · (B^v − 1)   with A = 0.05 s, B = 1.072, v ∈ 0..255
 *
 * v=0 is instant, small gaps resolve to ~70 ms, every gap is stored with
 * ≤ ~7% relative error, and v=255 tops out at ≈ 29 days (longer gaps clamp).
 * Constant cost: a full game's timing is exactly one byte per move.
 * ========================================================================== */

import { LogType } from "../common/types";
import { base32Encode, base32Decode } from "./codec";
import { INFO_TYPES } from "./core";
import { LOG_TYPE } from "../common/types";

export const EXTRAS_VERSION = 1;
const FLAG_NAMES = 1;
const FLAG_TIMES = 2;

const TIME_A = 0.05; // seconds
const TIME_B = 1.072;
const MAX_NAME_BYTES = 48;

export interface ReplayExtras {
    /** one per seat, '' when absent */
    names: string[] | null;
    /** unix seconds of GAME_START */
    startTime: number | null;
    /** seconds since the previous move, one per information-bearing move */
    moveGaps: number[] | null;
}

/* ------------------------------ time scaling ------------------------------ */

export function quantizeGap(seconds: number): number {
    const s = Math.max(0, seconds);
    const v = Math.round(Math.log(1 + s / TIME_A) / Math.log(TIME_B));
    return Math.max(0, Math.min(255, v));
}

export function dequantizeGap(v: number): number {
    return TIME_A * (Math.pow(TIME_B, v) - 1);
}

/* ----------------------------- container split ---------------------------- */

export function splitReplayCode(code: string): {
    moves: string;
    extras: string | null;
} {
    const i = code.indexOf("-");
    if (i < 0) return { moves: code, extras: null };
    return { moves: code.slice(0, i), extras: code.slice(i + 1) || null };
}

export function joinReplayCode(moves: string, extras: string | null): string {
    return extras ? `${moves}-${extras}` : moves;
}

/* -------------------------------- encoding -------------------------------- */

function utf8Encode(s: string): number[] {
    // TextEncoder exists in browsers, Deno and Node >= 11
    return Array.from(new TextEncoder().encode(s));
}
function utf8Decode(bytes: Uint8Array): string {
    return new TextDecoder().decode(bytes);
}

/**
 * Build the extras blob. `names` in seat order; `moveTimes` are absolute unix
 * seconds of GAME_START followed by each information-bearing move, in order
 * (use moveTimesFromLogs to extract them from game logs).
 */
export function encodeExtras(
    names: string[] | null,
    moveTimes: number[] | null,
): string {
    const out: number[] = [EXTRAS_VERSION, 0];
    let flags = 0;

    if (names && names.length > 0) {
        flags |= FLAG_NAMES;
        for (const name of names) {
            // usernames are arbitrary Unicode (signup only uppercases them) —
            // NUL termination is still safe because 0x00 never appears inside
            // a UTF-8 multi-byte sequence. Cap by trimming whole code points
            // (Array.from splits on code points, never mid-surrogate-pair).
            let points = Array.from(name.replace(/\0/g, ""));
            let bytes = utf8Encode(points.join(""));
            while (bytes.length > MAX_NAME_BYTES) {
                points = points.slice(0, -1);
                bytes = utf8Encode(points.join(""));
            }
            out.push(...bytes, 0);
        }
    }

    if (moveTimes && moveTimes.length >= 1) {
        flags |= FLAG_TIMES;
        const start = Math.max(0, Math.floor(moveTimes[0]));
        for (let i = 4; i >= 0; i--) out.push(Math.floor(start / 256 ** i) % 256);
        let prev = moveTimes[0];
        for (let i = 1; i < moveTimes.length; i++) {
            out.push(quantizeGap(moveTimes[i] - prev));
            prev = moveTimes[i];
        }
    }

    out[1] = flags;
    return base32Encode(Uint8Array.from(out));
}

/**
 * Parse an extras blob. `playerCount` and `moveCount` come from the decoded
 * moves payload (extras carry no redundant counts). Throws on malformed data;
 * callers should treat extras as best-effort decoration around the moves.
 */
export function decodeExtras(
    extras: string,
    playerCount: number,
    moveCount: number,
): ReplayExtras {
    const b = base32Decode(extras);
    if (b.length < 2) throw new Error("extras: truncated header");
    const version = b[0];
    if (version !== EXTRAS_VERSION)
        throw new Error(`extras: unsupported version ${version}`);
    const flags = b[1];
    let pos = 2;

    let names: string[] | null = null;
    if (flags & FLAG_NAMES) {
        names = [];
        for (let p = 0; p < playerCount; p++) {
            const start = pos;
            while (pos < b.length && b[pos] !== 0) pos++;
            if (pos >= b.length) throw new Error("extras: unterminated name");
            names.push(utf8Decode(b.slice(start, pos)));
            pos++; // skip the NUL
        }
    }

    let startTime: number | null = null;
    let moveGaps: number[] | null = null;
    if (flags & FLAG_TIMES) {
        if (pos + 5 > b.length) throw new Error("extras: truncated start time");
        startTime = 0;
        for (let i = 0; i < 5; i++) startTime = startTime * 256 + b[pos++];
        moveGaps = [];
        for (let i = 0; i < moveCount && pos < b.length; i++) {
            moveGaps.push(dequantizeGap(b[pos++]));
        }
        // base32 padding can leave a stray trailing byte; a count mismatch
        // beyond that means a corrupt blob
        if (moveGaps.length < moveCount && moveGaps.length > 0) {
            throw new Error(
                `extras: ${moveGaps.length} gaps for ${moveCount} moves`,
            );
        }
    }

    return { names, startTime, moveGaps };
}

/* ------------------------- extracting times server-side ------------------- */

/**
 * Pull [GAME_START time, each info-move time...] (unix seconds) out of a
 * session's logs — same session slicing and move filter as the move encoder.
 */
export function moveTimesFromLogs(
    logs: { log_type: LogType; created_at: string }[],
): number[] {
    let session = logs;
    for (let i = logs.length - 1; i >= 0; i--) {
        if (logs[i].log_type === LOG_TYPE.GAME_START) {
            session = logs.slice(i);
            break;
        }
    }
    const times: number[] = [];
    for (const l of session) {
        if (l.log_type === LOG_TYPE.GAME_START || INFO_TYPES.includes(l.log_type)) {
            const t = Date.parse(l.created_at) / 1000;
            times.push(Number.isFinite(t) ? t : times[times.length - 1] ?? 0);
        }
    }
    return times;
}
