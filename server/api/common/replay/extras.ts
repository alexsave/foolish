/* =============================================================================
 * Replay extras: player names + move timestamps (optional URL section)
 * =============================================================================
 * THE CODEC IS THE KERNEL'S (c/src/replay_extras.h). This file is the base32
 * and log-timestamp adapter around it: it turns the blob into the text half of
 * a URL and a session's log rows into the gaps the blob wants, and knows
 * nothing about the bytes in between. It used to be a second implementation of
 * the format, kept in step with a Swift third by a parity test - #113.
 *
 * The move integer (encode.ts/decode.ts) is deliberately untouched - extras
 * are a SEPARATE blob appended to the URL code after a dash:
 *
 *     WWW.FOOLISH.CARDS/<base32 moves>                 (moves only)
 *     WWW.FOOLISH.CARDS/<base32 moves>-<base32 extras> (full)
 *
 * '-' is in the QR alphanumeric charset and not in the base32 alphabet, so
 * the full form still QR-encodes densely, and the moves-only code is always
 * just the prefix before the dash - store the full form, cut to get the
 * small one.
 * ========================================================================== */

import { LogType } from "@api/core/types.ts";
import { base32Encode, base32Decode } from "./codec.ts";
import { INFO_TYPES } from "./core.ts";
import { LOG_TYPE } from "@api/core/types.ts";
import {
    kernelReplayExtrasEncode,
    kernelReplayExtrasDecode,
} from "@sdk/ts/wasm/bots.ts";

export interface ReplayExtras {
    /** one per seat, '' when absent */
    names: string[] | null;
    /** unix seconds of GAME_START */
    startTime: number | null;
    /** seconds since the previous move, one per information-bearing move */
    moveGaps: number[] | null;
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
    return base32Encode(kernelReplayExtrasEncode(names, startTime, gaps));
}

/**
 * The blob as RAW BYTES, for the one caller that stores it rather than putting
 * it in a URL (game_snapshots.extras). The share code is derived from the
 * column client-side, so base32 would only be a round trip.
 */
export function encodeExtrasBytes(
    names: string[] | null,
    moveTimes: number[] | null,
): Uint8Array {
    const gaps = moveTimes && moveTimes.length >= 1 ? gapsFrom(moveTimes) : null;
    return kernelReplayExtrasEncode(names, gaps ? moveTimes![0] : null, gaps);
}

function gapsFrom(moveTimes: number[]): number[] {
    const gaps: number[] = [];
    for (let i = 1; i < moveTimes.length; i++) {
        gaps.push(Math.max(0, moveTimes[i] - moveTimes[i - 1]));
    }
    return gaps;
}

/**
 * Convenience wrapper over absolute unix-seconds move times - [GAME_START,
 * move, move, ...] as produced by moveTimesFromLogs. Differences of absolute
 * doubles resolve to ~us near the current epoch; finer than that, use
 * encodeExtrasFromGaps.
 */
export function encodeExtras(
    names: string[] | null,
    moveTimes: number[] | null,
): string {
    if (!moveTimes || moveTimes.length < 1) {
        return encodeExtrasFromGaps(names, null, null);
    }
    return encodeExtrasFromGaps(names, moveTimes[0], gapsFrom(moveTimes));
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
    return kernelReplayExtrasDecode(base32Decode(extras), playerCount, moveCount);
}

/* ------------------------- extracting times server-side ------------------- */

/**
 * Pull [GAME_START time, each info-move time...] (unix seconds) out of a
 * session's logs - same session slicing and move filter as the move encoder.
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
