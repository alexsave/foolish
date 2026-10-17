// Memory regression suite for the wasm kernel (`npm run test:mem`).
// Born of the two production "Memory limit exceeded" outages of 2026-07-06 on
// the Supabase edge functions, which ran the bots in wasm (150MB heap + 150MB
// external, and wasm memory counts as external). The edge functions are gone -
// the server is c_base, native - so what is left here is the browser's module
// (its size and its static memory) and the bots' bump-allocator discipline,
// which still matters wherever the bots run in wasm - the Oracle's module,
// public/oracle.wasm.gz, where octogen deliberates in a visitor's tab:
//
//   1. Embed parse blowup. The wasm embeds were ~2300 base64 chunks joined
//      with '+'; V8 folds the chain pairwise at parse time, allocating ~N
//      intermediate strings (~300MB transient for the 274KB bots embed) —
//      edge workers died DURING MODULE IMPORT, before any bot ran. Guarded
//      by a format check plus importing each embed in a child node whose
//      old-space is capped far below the old transient cost.
//
//   2. Wasm memory growth. The MC families' endgame solvers malloc'd
//      per-family scratch on the wasm bump allocator (free is a no-op), so
//      memory grew per family exercised (46->87->127MB observed). Guarded by
//      deliberating every decision of several games in the Oracle's module and
//      asserting its memory is bounded AND exactly flat once the first game
//      has warmed it. (It played every MC family in the whole-kernel wasm test
//      module until that went: the Oracle runs octogen alone, and the other
//      families run nowhere in wasm.)

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { resolve } from 'node:path';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

// The kernel ships as a gzip STATIC ASSET - a real binary, so concern #1
// (parse-time concat blowup) is gone by construction:
//
//   web.wasm.gz    the browser's link, fetched on first visit: the kernel's
//                  objects under the exports the browser calls (c/Makefile,
//                  WASM_WEB_NAMES), so wasm-ld drops the Monte-Carlo brains,
//                  cordite's solver working set and the whole C Table.
//
// (There was a second, bots.wasm.gz, the server's whole-kernel link, until the
// server became native. The measurements below that name it are its history.)
//
// WHAT THIS GATE MEASURES, AND WHY IT IS NOT THE GZIP SIZE ANY MORE.
//
// It used to be `buf.length < 80 * 1024` over a COMMITTED bots.wasm.gz. That
// line described one laptop. The module is built by the lane that ships it now
// (scripts/wasm_build.sh), so the question "which toolchain's bytes is the
// budget about" had to be answered, and the measurements say a gzip-byte budget
// this tight cannot be one number.
//
// The compiler is not the problem. With the toolchain pinned (clang 22.1.8 +
// binaryen 130, shared/scripts/ci_llvm.sh) the RAW module is byte-identical on macOS
// arm64, Linux arm64 and Linux x86_64. The COMPRESSOR is the problem. gzip -9 -n
// over one identical 191,485 B bots.wasm gave:
//
//   Apple gzip 487.0.1 (macOS)    81,892 B    <- what the committed file was
//   GNU gzip 1.12 (ubuntu)        82,043 B    +151 B  <- what CI ships
//   node 26 zlib level 9          81,892 B
//   node 20 zlib level 9          82,468 B    +576 B
//
// A 576 B spread, against 28 B of headroom under 81,920. The budget's noise was
// twenty times its margin, so the same kernel passed or failed on which machine
// asked - and on the toolchain that actually shipped the bytes the 80 KiB line
// was exceeded by 123 B.
//
// THAT LINE IS NOW MET WITH ROOM, and not by shrinking the kernel: the browser
// stopped downloading the server's link of it. web.wasm.gz is 31,050 B, so the
// download the 80 KiB budget was always about clears it by 50,870 B. The RAW
// pins below are what measure drift, because raw bytes are stable everywhere;
// the download number stays TRACKED as well as pinned - scripts/collect_metrics.mjs
// reports each module's gzip size and metrics.yml diffs it head-vs-base on every
// pull request, which is a better instrument for a few hundred bytes than a
// boolean ever was. Each gz ceiling here is deliberately wide of its measured
// value: it catches a base64 embed coming back or a blowup, not a drift.
//
// THE RAW PINS ARE BLOWUP DETECTORS, NOT A RATCHET. Owner's call, after the
// beats model tripped a 192,000 B pin by 668 B and the fix was two pins and a
// paragraph: "as long as the wasm doesn't double in size, it's blocking us from
// shipping fixes and features". So these sit at 2x the measured module and catch
// what the gz ceilings below catch - a base64 embed coming back, a runtime
// arriving, a buffer sized from a variable that turned out not to be constant -
// and nothing smaller.
//
// DRIFT IS STILL WATCHED, just not here and not as a gate. scripts/collect_metrics.mjs
// reports each module's gzip size and metrics.yml diffs it head-vs-base on every
// pull request, which the note above already calls "a better instrument for a few
// hundred bytes than a boolean ever was". What is given up is the one-way ratchet
// of Part 3 of ../docs/ARCHITECTURE_AS_A_PATTERN.md - the discipline that banked the
// browser's 62% cut as a lower pin instead of headroom to spend again. Re-pinning
// after a deliberate win is now something a person chooses to do, not something
// CI makes them do. It also makes this gate runnable off CI again: at 192,000 B
// it was red on any toolchain but the pinned one (this repo's own container,
// clang 18, builds a 196,196 B bots.wasm), so the only machine that could answer
// it was the one you were waiting on.
// The BROWSER's link, and the one the download budget is about. Same rule:
// it had 702 B of raw room and 2,742 B of gz room, which is about one feature,
// and 576 B of the gz figure is compressor spread rather than kernel.
const WEB_RAW_MAX = 137_000;        // ~68,100 B today; 2x that
const WEB_GZ_MAX = 61 * 1024;       // 31,050 B today; 2x
test('the kernel ships as small gzip static assets (not base64 embeds)', () => {
    for (const [rel, rawMax, gzMax] of [
        ['sdk/ts/wasm/web.wasm.gz', WEB_RAW_MAX, WEB_GZ_MAX],
    ] as const) {
        const buf = readFileSync(resolve(rel));
        assert.equal(buf[0], 0x1f, `${rel} is not gzip`);
        assert.equal(buf[1], 0x8b, `${rel} is not gzip`);

        // The raw size comes from inflating the shipped file rather than reading
        // c/build, so this test still has exactly ONE input per module. A second
        // path would be a second thing that can be stale, and c/build is wiped by
        // any `--check` run.
        const raw = gunzipSync(buf);
        assert.ok(raw.length <= rawMax,
            `${rel} is ${raw.length} B raw, over the ${rawMax} B pin.\n`
            + 'This is the kernel getting bigger, and it is measured on the raw module\n'
            + 'because that is byte-identical on every platform this repo builds on -\n'
            + 'see the note above this test. Going UP is a regression: find what was\n'
            + 'added. If it is deliberate, raise this pin IN THE SAME COMMIT and say\n'
            + 'what bought the bytes, then check the gzip delta metrics.yml posts on\n'
            + 'the pull request - that is the number a visitor actually downloads.\n'
            + 'On web.wasm, check FIRST whether a new export in c/Makefile WASM_WEB_NAMES\n'
            + 'rooted code the browser does not run.');

        assert.ok(buf.length <= gzMax,
            `${rel} is ${buf.length} B, over the ${gzMax} B ceiling.\n`
            + 'This ceiling is wide on purpose (the compressor alone moves this number\n'
            + `by up to 576 B) - it is here to catch a base64 embed coming back or a\n`
            + 'blowup, so being over it means something large arrived, not that the\n'
            + 'kernel drifted. The raw pin above is the one that measures drift.');
    }
});

const PAGE = 65536;
// Parse the memory section's (min, max) page limits straight from the binary —
// the pin is a link-time fact, so assert on the module, not a live instance.
function memLimits(wasm: Uint8Array): { min: number; max: number | null } {
    let p = 8; // skip the 8-byte module header
    const leb = () => { let r = 0, s = 0, b: number; do { b = wasm[p++]; r |= (b & 0x7f) << s; s += 7; } while (b & 0x80); return r >>> 0; };
    while (p < wasm.length) {
        const id = wasm[p++], len = leb(), end = p + len;
        if (id === 5) { // memory section
            leb(); // count (1)
            const flags = leb(), min = leb();
            return { min, max: (flags & 1) ? leb() : null };
        }
        p = end;
    }
    throw new Error('no memory section');
}

test("web.wasm declared INITIAL memory is 25 pages", () => {
    // A LINKER FACT, not a second budget. wasm-ld keeps a static only if
    // something an export reaches refers to it, so dropping the bot bridge and
    // the C Table from the browser's export list drops their buffers with their
    // code: the cordite solver working set, the world-log slots, g_io's 400 KiB
    // log-import staging (the session log the belief bots filter - a browser
    // imports no logs) and the table's own scratch, deal-sized snapshot store
    // included. 34 - 26 = 8 pages, 524,288 B of linear memory a tab no longer
    // reserves, on top of the 51,241 B of download (83,111 - 31,870 gz) and the
    // 124,657 B of module (193,966 - 69,309 raw) it no longer compiles.
    //
    // Pinned rather than merely reported:
    // going UP means a browser call site pulled a server-sized buffer across,
    // which is the moment to ask whether the call belongs there at all. Going
    // DOWN is a win - re-pin it here and say what was freed.
    //
    // 24, LOWERED from 26: wasm_msg_decode and wasm_msg_header_ptr left the
    // browser's export list when the web's /m/ route (their one caller) went
    // with the static export, and the linker dropped the 131,072 B of statics
    // only the FMSG decode reached.
    //
    // 25, RAISED from 24 on purpose: the browser now carries the backend's client
    // (c/src/netclient.c, exported as wasm_net_*; stage 3 of c_base/DESIGN.md
    // "Binary surface") in place of the third-party SDK. Its statics are the session,
    // the request it builds, one decoded stream message (g_frame, 32 KiB: a text
    // frame has to be decoded somewhere), the subscriptions and the per-game
    // version gates (NC_GATES_MAX 64, cut from 128 so bots.wasm keeps its
    // 34-page line), about 54 KiB in all.
    const wasm = new Uint8Array(gunzipSync(readFileSync(resolve('sdk/ts/wasm/web.wasm.gz'))));
    const { min } = memLimits(wasm);
    assert.equal(min, 25,
        `web.wasm initial memory is ${min} pages (${min * PAGE} B); expected 25. `
        + 'Up means the browser\'s link started keeping a static it used to drop - look at what '
        + 'was added to c/Makefile WASM_WEB_NAMES, because an export is a GC root and roots the '
        + 'data its code reads. Down means something was freed: lower this pin in the same commit.');
});

test("the Oracle's wasm memory is bounded and flat across games", async () => {
    // The Oracle's own module and its own bridge, as the replay screen runs
    // them (src/oracle/oracleBridge.ts), deliberating every decision of a game.
    // The games are played by the kernel the server runs (the native table
    // host, e2e/helpers/bot_table.ts), cut to their replay codes and walked by
    // the web's replay reader, exactly as e2e/oracle_replay.test.ts does.
    const { OracleInstance } = await import('../../src/oracle/oracleBridge.ts');
    const { ORACLE_KNOBS } = await import('../../src/oracle/types.ts');
    const { buildOracleJob, findDecisionIndex } = await import('../../src/oracle/replayOracleInput.ts');
    const { buildReplayFrames } = await import('../../src/replay/frames.ts');
    const { gunzip } = await import('../../sdk/ts/wasm/gunzip.ts');
    const { playBotTable, seedBytes } = await import('../helpers/bot_table.ts');

    const inst = new OracleInstance();
    await inst.init(gunzip(new Uint8Array(readFileSync(resolve('public/oracle.wasm.gz')))));
    const bytes = () => inst.ex.memory.buffer.byteLength;

    let decisions = 0;
    const deliberate = (id: string, brains: string[], seed: number) => {
        const { code } = playBotTable(brains, seedBytes(brains.length, seed), { gameId: id });
        const frames = buildReplayFrames(code, id, null);
        const seen = new Set<number>();
        for (let idx = 0; idx < frames.length; idx++) {
            const j = findDecisionIndex(frames, idx);
            if (j == null || seen.has(j)) continue;
            seen.add(j);
            const job = buildOracleJob(frames, code, idx, true, id);
            assert.ok(job, `a job at step ${idx} of ${id}`);
            inst.setKnobs({ ...ORACLE_KNOBS, w1: 4 });
            assert.ok('record' in inst.analyzeOnce(job!, 0x51 + j), `answered at j=${j} (${id})`);
            decisions++;
        }
    };

    // Game 1 warms the solver's scratch and the transposition table.
    deliberate('mem1', ['octogen', 'cordite'], 1);
    const afterFirst = bytes();
    // The kernel plus its scratch must stay small: a tab running the Oracle
    // shares its memory with everything else on the page.
    assert.ok(afterFirst <= 16 * 1048576,
        `the Oracle's wasm memory after one game is ${(afterFirst / 1048576) | 0}MB; budget regression`);

    // Every further game - other seat counts, other boards - must reuse the
    // SAME scratch: exactly flat. This is the guard that would have caught a
    // per-call or per-shape allocation on the bump allocator, which never frees.
    deliberate('mem2', ['cordite', 'octogen'], 2);
    deliberate('mem3', ['blackpowder', 'firecracker', 'octogen'], 3);
    deliberate('mem4', ['handwritten', 'random', 'octogen', 'firecracker'], 4);
    assert.ok(decisions > 40, `only ${decisions} decisions were deliberated`);
    assert.equal(bytes(), afterFirst,
        `the Oracle's wasm memory grew ${((bytes() - afterFirst) / 1048576) | 0}MB across games; per-call allocations are back`);
});
