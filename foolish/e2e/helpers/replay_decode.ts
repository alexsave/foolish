// replay_decode.ts - a replay code decoded by the kernel, as tests and tools read it.
//
// The header is the kernel's ReplaySummary (c/src/replay_steps.h), read through its
// generated snapshot reader; the log stream is replay_decode's, one record at a time
// through replay_decoded_log (c/src/replay.h) and the generated ReplayDecodedLog
// accessors. No byte of the decoder's output is read here. It runs on a private
// instance of the test build (e2e/helpers/bots_test_wasm.ts), which is the only
// module that exports the log reader: no shipped host reads a whole log stream.
import * as G from '../../sdk/ts/gen/game_layout.bots.ts';
import { LAYOUT_HASH } from '../../sdk/ts/gen/layout_hash.bots.ts';
import { CARD_NONE_SUIT, CARD_NONE_VALUE, memOf as viewMemOf, readReplaySummary, type ReplaySummary_Snap, type Card_Snap } from '../../sdk/ts/gen/view_layout.bots.ts';
import { assertLayoutHash } from '../../sdk/ts/wasm/layout_hash.ts';
import { kernelB32Decode, kernelReplayExtrasDecode, kernelReplayLinkParse } from '../../sdk/ts/wasm/bots.ts';
import { botsTestWasm } from './bots_test_wasm.ts';

interface DecodeExports {
    memory: WebAssembly.Memory;
    wasm_layout_hash(): number;
    wasm_replay_io_ptr(): number;
    wasm_replay_io_cap(): number;
    wasm_replay_summary(codeLen: number): number;
    wasm_replay_decoded_open(codeLen: number): number;
    wasm_replay_decoded_next(): number;
}

let cached: DecodeExports | null = null;
function kernel(): DecodeExports {
    if (!cached) {
        const ex = new WebAssembly.Instance(new WebAssembly.Module(botsTestWasm() as BufferSource), {}).exports as unknown as DecodeExports;
        assertLayoutHash('bots_test.wasm', ex, LAYOUT_HASH, 'sdk/ts/gen/layout_hash.bots.ts');
        cached = ex;
    }
    return cached;
}

/** One record of the decoded log stream. `seat` and `defender` are -1 for none; `type` is a LOG_* constant. */
export interface DecodedLog {
    readonly type: number;
    readonly seat: number;
    readonly defender: number;
    readonly pairs: readonly { readonly primary: Card_Snap; readonly target: Card_Snap | null }[];
}

export interface DecodedReplay {
    /** The decoder's header: version, trump, opener, fool, elimination order, discard count, moves. */
    readonly summary: ReplaySummary_Snap;
    /** The full reconstructed event stream, GAME_START through the final event. */
    readonly logs: readonly DecodedLog[];
}

function replayError(what: string, rc: number): Error {
    const name = Object.entries(G).find(([k, v]) => k.startsWith('REPLAY_E') && !k.startsWith('REPLAY_EXTRAS_') && v === -rc)?.[0];
    return new Error(`replay ${what}: ${name ?? rc}`);
}

/** The code (the replay integer's big-endian bytes) decoded, or an Error naming the kernel's REPLAY_E* refusal. */
export function decodeReplayCode(code: Uint8Array): DecodedReplay {
    const ex = kernel();
    if (code.length > ex.wasm_replay_io_cap()) throw replayError('decode', -G.REPLAY_ECAP);
    const put = () => new Uint8Array(ex.memory.buffer).set(code, ex.wasm_replay_io_ptr());
    put();
    const at = ex.wasm_replay_summary(code.length);
    if (at <= 0) {
        put();
        const rc = ex.wasm_replay_decoded_open(code.length);
        throw replayError('decode', rc < 0 ? rc : -G.REPLAY_EHEADER);
    }
    const summary = readReplaySummary(viewMemOf(ex.memory.buffer), at);
    put();
    const rc = ex.wasm_replay_decoded_open(code.length);
    if (rc < 0) throw replayError('decode', rc);
    const logs: DecodedLog[] = [];
    for (;;) {
        const p = ex.wasm_replay_decoded_next();
        if (p === 0) break;
        if (p < 0) throw replayError('log stream', p);
        const m = G.memOf(ex.memory.buffer);
        const card = (c: number) => ({ suit: G.Card_get_suit(m, c), value: G.Card_get_value(m, c) });
        const pairs = [];
        for (let i = 0; i < G.ReplayDecodedLog_get_n_pairs(m, p); i++) {
            const t = G.ReplayDecodedLog_target_at(p, i);
            const target = card(t);
            pairs.push({ primary: card(G.ReplayDecodedLog_primary_at(p, i)),
                target: target.suit === CARD_NONE_SUIT && target.value === CARD_NONE_VALUE ? null : target });
        }
        logs.push({ type: G.ReplayDecodedLog_get_log_type(m, p), seat: G.ReplayDecodedLog_get_seat(m, p),
            defender: G.ReplayDecodedLog_get_defender(m, p), pairs });
    }
    return { summary, logs };
}

/** A pasted share link or code, decoded, with the names its extras carry (null when it has none). */
export function decodeReplayLink(link: string): DecodedReplay & { names: readonly string[] | null; moveGaps: readonly number[] | null } {
    // The kernel's link parser returns the moves code and drops the extras after '-'.
    const decoded = decodeReplayCode(kernelB32Decode(kernelReplayLinkParse(link)));
    const extras = /-([A-Za-z2-7]+)\s*$/.exec(link.split(/[?#]/)[0].replace(/\/+\s*$/, ''))?.[1] ?? '';
    if (!extras) return { ...decoded, names: null, moveGaps: null };
    const x = kernelReplayExtrasDecode(kernelB32Decode(extras), decoded.summary.numPlayers, decoded.summary.moves);
    return { ...decoded, names: x.names, moveGaps: x.moveGaps };
}
