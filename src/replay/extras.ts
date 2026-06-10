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
 * Extras blob layout (version 2):
 *   byte 0          version (2)
 *   byte 1          flags: bit0 = names section, bit1 = times section
 *   names section   playerCount × (UTF-8 bytes + 0x00). playerCount comes
 *                   from the decoded moves payload.
 *   times section   1 byte: scale exponent e — the time unit is 2^(e−64) s
 *                   5 bytes: game-start unix time, seconds, big-endian
 *                   then 1 byte per information-bearing move (ATTACK / COVER /
 *                   PASS / PICKUP / GOOD, in move order): the gap since the
 *                   previous move, log-quantized (see below).
 *
 * TIME SCALING. Move gaps are wildly non-uniform AND wildly game-dependent:
 * humans pause seconds to weeks, simulations step in nanoseconds. So the
 * encoding is scale-free. Each gap is one byte on a μ-law-style log curve:
 *
 *     gap(v) = unit · (B^v − 1)   with B = 1.072, v ∈ 0..255
 *
 * giving ~7.7 decades of dynamic range at ≤ ~7% relative error for one byte
 * per move — and `unit` itself is stored per blob as a power of two,
 * 2^(e−64) seconds (e ∈ 0..255 spans ~5e-20 s to ~3e57 s). The encoder
 * auto-fits: the smallest unit whose curve still reaches the game's largest
 * gap. A bot blitz quantizes at microsecond resolution, a correspondence
 * game at ~50 ms — same wire size either way.
 * ========================================================================== */

import { LogType } from "../common/types";
import { base32Encode, base32Decode } from "./codec";
import { INFO_TYPES } from "./core";
import { LOG_TYPE } from "../common/types";

export const EXTRAS_VERSION = 2;
const FLAG_NAMES = 1;
const FLAG_TIMES = 2;

const TIME_B = 1.072;
const TIME_RANGE = Math.pow(TIME_B, 255) - 1; // ≈ 4.8e7: the curve's reach in units
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

/** unit in seconds for a stored scale exponent */
export function unitFor(scaleExp: number): number {
    return Math.pow(2, scaleExp - 64);
}

/** smallest exponent whose curve still reaches the game's largest gap */
export function pickScaleExp(maxGapSeconds: number): number {
    if (!(maxGapSeconds > 0)) return 64; // degenerate: all-zero gaps, unit = 1 s
    const e = Math.ceil(Math.log2(maxGapSeconds / TIME_RANGE)) + 64;
    return Math.max(0, Math.min(255, e));
}

export function quantizeGap(seconds: number, unit: number): number {
    const s = Math.max(0, seconds);
    const v = Math.round(Math.log(1 + s / unit) / Math.log(TIME_B));
    return Math.max(0, Math.min(255, v));
}

export function dequantizeGap(v: number, unit: number): number {
    return unit * (Math.pow(TIME_B, v) - 1);
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
 * Build the extras blob from a start time plus per-move gaps. This is the
 * precision-preserving entry point: gaps are stored relative, so nanosecond
 * resolution survives even though an ABSOLUTE unix-seconds double cannot
 * represent it (the ULP near 2e9 s is ~240 ns). High-resolution producers
 * (simulations) should call this directly with their raw gaps.
 */
export function encodeExtrasFromGaps(
    names: string[] | null,
    startTime: number | null,
    gaps: number[] | null,
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

    if (startTime !== null && gaps !== null) {
        flags |= FLAG_TIMES;
        const maxGap = gaps.reduce((m, g) => Math.max(m, g), 0);
        const scaleExp = pickScaleExp(maxGap);
        const unit = unitFor(scaleExp);
        out.push(scaleExp);
        const start = Math.max(0, Math.floor(startTime));
        for (let i = 4; i >= 0; i--) out.push(Math.floor(start / 256 ** i) % 256);
        for (const g of gaps) out.push(quantizeGap(g, unit));
    }

    out[1] = flags;
    return base32Encode(Uint8Array.from(out));
}

/**
 * Convenience wrapper over absolute unix-seconds move times — [GAME_START,
 * move, move, ...] as produced by moveTimesFromLogs. Differences of absolute
 * doubles resolve to ~μs near the current epoch; finer than that, use
 * encodeExtrasFromGaps.
 */
export function encodeExtras(
    names: string[] | null,
    moveTimes: number[] | null,
): string {
    if (!moveTimes || moveTimes.length < 1) {
        return encodeExtrasFromGaps(names, null, null);
    }
    const gaps: number[] = [];
    for (let i = 1; i < moveTimes.length; i++) {
        gaps.push(Math.max(0, moveTimes[i] - moveTimes[i - 1]));
    }
    return encodeExtrasFromGaps(names, moveTimes[0], gaps);
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
        if (pos + 6 > b.length) throw new Error("extras: truncated time header");
        const unit = unitFor(b[pos++]);
        startTime = 0;
        for (let i = 0; i < 5; i++) startTime = startTime * 256 + b[pos++];
        moveGaps = [];
        for (let i = 0; i < moveCount && pos < b.length; i++) {
            moveGaps.push(dequantizeGap(b[pos++], unit));
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
