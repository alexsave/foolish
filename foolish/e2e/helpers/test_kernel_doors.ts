// A kernel door only tests open, on the test build (bots_test.wasm, through the
// fixtures' instance in e2e/helpers/table_fixture.ts).
//
// It lived in sdk/ts/wasm/bots.ts while that file could load a whole-kernel
// module in Node (bots.wasm, the old server link). It loads web.wasm everywhere
// now, which does not export this, and no browser path calls it. The animation
// doors that were here (the stale release and the reversal's order) are asked
// natively now, in c/tests/tests.c.

import { fixtureExports } from './table_fixture.ts';

function ok(rc: number, refusal: (code: number) => Error): number {
    if (rc < 0) throw refusal(rc);
    return rc;
}

/**
 * The replay code out of whatever a person pasted - the kernel's
 * replay_link_parse. Building a link and reading one back are two halves of the
 * same format, and this is the half that has to REFUSE: replay_b32_decode is
 * deliberately tolerant, so an unstripped "https" prefix would decode to a
 * different game rather than fail. Throws when the input is not a code.
 */
export function kernelReplayLinkParse(url: string): string {
    const ex = fixtureExports();
    const text = new TextEncoder().encode(url);
    let at = ex.wasm_replay_io_ptr();
    if (text.length + 1 > ex.wasm_replay_io_cap()) throw new Error('link: does not fit the kernel IO buffer');
    const u8 = new Uint8Array(ex.memory.buffer);
    u8.set(text, at);
    u8[at + text.length] = 0;
    const n = ok(ex.wasm_replay_link_parse(text.length), () => new Error(`not a replay code: "${url}"`));
    at = ex.wasm_io_ptr();
    return new TextDecoder().decode(new Uint8Array(ex.memory.buffer).slice(at, at + n));
}
