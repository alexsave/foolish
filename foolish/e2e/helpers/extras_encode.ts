// The replay extras ENCODER, for tests: names and move timing into the blob
// behind the dash of a share link (c/src/replay_extras.h).
//
// No shipped host of bots.wasm encodes one through this entry: a game's extras
// are written by the C Table when it finishes (wasm_table_replay_extras) and
// the phone links the encoder natively. What is left is a test that needs a
// blob to decode, to measure against the frozen format (./extras_format_v2.ts)
// or to put behind a code a page reads - so the entry is the TEST build's
// (c/Makefile WASM_BOTS_UNSHIPPED_API) and these are its callers. The names
// and times cross as the kernel's own struct, through the generated writer.
import * as V from '../../sdk/ts/gen/view_layout.bots.ts';
import * as M from '../../sdk/ts/gen/msg_layout.bots.ts';
import { ReplayExtrasErrorText } from '../../sdk/ts/gen/errtext/replay_extras.ts';
import { kernelB32Encode } from '../../sdk/ts/wasm/bots.ts';
import { fixtureExports } from './table_fixture.ts';

/**
 * Roster + timing -> the raw extras blob. `names` must be as wide as the table
 * (a reader takes the seat count from the decoded moves); unnamed seats are ''.
 * Passing null for either half leaves that section out.
 */
export function kernelReplayExtrasEncode(names: string[] | null, startTime: number | null,
                                         gaps: number[] | null): Uint8Array {
    const ex = fixtureExports();
    const hasNames = (names?.length ?? 0) > 0;
    const hasTimes = startTime !== null && gaps !== null;
    M.writeReplayExtras(V.memOf(ex.memory.buffer), ex.wasm_replay_extras_ptr(), {
        flags: (hasNames ? M.REPLAY_EXTRAS_FLAG_NAMES : 0) | (hasTimes ? M.REPLAY_EXTRAS_FLAG_TIMES : 0),
        names: hasNames ? names!.map((text) => ({ text })) : [],
        startTime: hasTimes ? startTime! : 0,
        gaps: hasTimes ? gaps! : [],
    });
    const n = ex.wasm_replay_extras_encode();
    if (n < 0) throw new Error(ReplayExtrasErrorText.find((row) => row.code === n)?.text ?? `extras: kernel error ${n}`);
    const at = ex.wasm_io_ptr();
    return new Uint8Array(ex.memory.buffer).slice(at, at + n);
}

/** The blob as the text half of a URL, from a start time plus per-move gaps. */
export function encodeExtrasFromGaps(names: string[] | null, startTime: number | null, gaps: number[] | null): string {
    return kernelB32Encode(kernelReplayExtrasEncode(names, startTime, gaps));
}

function gapsFrom(moveTimes: number[]): number[] {
    return moveTimes.slice(1).map((t, i) => Math.max(0, t - moveTimes[i]));
}

/** The blob as raw bytes (game_snapshots.extras), from absolute unix-seconds move times: [GAME_START, move, move, ...]. */
export function encodeExtrasBytes(names: string[] | null, moveTimes: number[] | null): Uint8Array {
    const gaps = moveTimes && moveTimes.length >= 1 ? gapsFrom(moveTimes) : null;
    return kernelReplayExtrasEncode(names, gaps ? moveTimes![0] : null, gaps);
}

/** The same, as base32. */
export function encodeExtras(names: string[] | null, moveTimes: number[] | null): string {
    return kernelB32Encode(encodeExtrasBytes(names, moveTimes));
}
