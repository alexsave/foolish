// A kernel door only tests open, in the native table host (through the
// fixtures' host in e2e/helpers/table_fixture.ts).
//
// It lived in sdk/ts/wasm/bots.ts while that file could load a whole-kernel
// module in Node (bots.wasm, the old server link). It loads web.wasm everywhere
// now, which does not export this, and no browser path calls it. The animation
// doors that were here (the stale release and the reversal's order) are asked
// natively now, in c/tests/tests.c.

import { fixtureHost } from './table_fixture.ts';

function ok(rc: number, refusal: (code: number) => Error): number {
    if (rc < 0) throw refusal(rc);
    return rc;
}

/**
 * The replay code out of whatever a person pasted - the kernel's
 * replay_link_parse. Building a link and reading one back are two halves of the
 * same format, and this is the half that has to REFUSE: b32_decode is
 * deliberately tolerant, so an unstripped "https" prefix would decode to a
 * different game rather than fail. Throws when the input is not a code.
 */
export function kernelReplayLinkParse(url: string): string {
    const host = fixtureHost();
    const ex = host.ex;
    const text = new TextEncoder().encode(url);
    if (text.length + 1 > ex.wasm_replay_io_cap()) throw new Error('link: does not fit the kernel IO buffer');
    const z = new Uint8Array(text.length + 1);
    z.set(text, 0);
    host.write(ex.wasm_replay_io_ptr(), z);
    const n = ok(ex.wasm_replay_link_parse(text.length), () => new Error(`not a replay code: "${url}"`));
    return new TextDecoder().decode(host.read(ex.wasm_io_ptr(), n));
}
