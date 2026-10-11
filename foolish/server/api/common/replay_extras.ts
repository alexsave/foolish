/* =============================================================================
 * Replay extras: player names + move timestamps (optional URL section)
 * =============================================================================
 * THE CODEC IS THE KERNEL'S (c/src/replay_extras.h). This file is what a page
 * that READS a code needs around it: the split at the dash and the blob out of
 * its base32. Writing one is the C Table's, when a game finishes
 * (wasm_table_replay_extras). It used to be a second implementation of the
 * format, kept in step with a Swift third by a parity test - #113.
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

import { kernelB32Decode, kernelReplayExtrasDecode } from "@sdk/ts/wasm/bots.ts";

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

/* -------------------------------- decoding -------------------------------- */

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
    return kernelReplayExtrasDecode(kernelB32Decode(extras), playerCount, moveCount);
}
