// Memory regression suite for the wasm bot stack (`npm run test:mem`).
// Guards the two production "Memory limit exceeded" outages of 2026-07-06,
// which ordinary tests missed because Node has no per-worker memory budget
// (edge functions get 150MB heap + 150MB external, and wasm memory counts
// as external):
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
//      playing full games across all MC families and asserting the bots.wasm
//      memory is bounded AND exactly flat once the first game has warmed it.
//
// The gold-path repro (real edge runtime, real memory budget) is
// e2e/edge_memtest/index.ts, run by .github/workflows/memory.yml.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { gunzipSync } from 'node:zlib';
import { resolve } from 'node:path';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

// The kernel ships as gzip STATIC ASSETS - real binaries, so concern #1
// (parse-time concat blowup) is gone by construction. There are TWO of them,
// and they are two LINKS of one object set rather than two kernels
// (c/Makefile, WASM_WEB_NAMES; e2e/wasm_web_link.test.ts holds them to one
// layout hash, one export surface and one answer):
//
//   bots.wasm.gz   the server-side link. The edge functions and Node read it
//                  off disk. Nobody downloads it.
//   web.wasm.gz    the browser's link, fetched on first visit. The same objects
//                  minus every export the browser does not call, so wasm-ld
//                  drops the Monte-Carlo brains, cordite's solver working set
//                  and the whole C Table.
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
// binaryen 130, scripts/ci_llvm.sh) the RAW module is byte-identical on macOS
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
// stopped downloading the server's link of it. web.wasm.gz is ~31 KB, so the
// download the 80 KiB budget was always about clears it by about 50 KB. The
// download number stays TRACKED as well as pinned - scripts/collect_metrics.mjs
// reports each module's gzip size and metrics.yml diffs it head-vs-base on every
// pull request, which is a better instrument for a few hundred bytes than a
// boolean ever was. Each ceiling here is deliberately wide of its measured
// value: it catches a base64 embed coming back or a blowup, not a drift.
//
// RAW BYTES ARE NOT STABLE ACROSS HOSTS EITHER, which is what this comment used
// to say they were, and the difference is smaller than the compressor spread but
// bigger than the margin it was left. One tree, one nominal toolchain version,
// both with binaryen 130:
//
//   macOS arm64, Homebrew clang 22.1.8              192,186 B raw
//   Linux x86_64, apt.llvm.org clang 22.1.8         ~191,900 B raw  (-~290 B)
//
// Same version string, different builds of it: apt.llvm.org's llvm-22 is a
// rolling snapshot inside the major (`++20260714014902+ca7933e47d3a`) and
// scripts/ci_llvm.sh pins LLVM_VERSION to the MAJOR, while a Mac follows its own
// Homebrew bottle. The two agreed when scripts/wasm_build.sh measured them and
// have since drifted apart.
//
// So the pins are set wide of the WORST host, the way the gz pins already were,
// rather than at the smallest measurement plus a sliver. At 192,000 the gate
// was red on a Mac and green in CI for the same commit, which is the one thing
// a gate must never do: an octogen change that cost 445 B landed under a pin it
// was already over, because the number it was compared against (191,490) had
// been copied from a c/Makefile comment rather than built.
const BOTS_RAW_MAX = 193_000;       // 192,186 B on the widest host: 814 B of
                                    // room, ~2.8x the measured host spread
const BOTS_GZ_MAX = 84 * 1024;      // 82,502 B on the widest host; clears the
                                    // worst compressor above by 3,089 B
// The BROWSER's link, and the one the download budget is about. Pinned at the
// measured size plus room for the compressor spread, NOT at the old 80 KiB
// line: Part 3 of docs/ARCHITECTURE_AS_A_PATTERN.md says to re-pin lower after
// each win so the ratchet turns one way, and leaving this at 80 KiB would have
// banked a 62% cut as 50 KB of silent headroom to spend again.
const WEB_RAW_MAX = 68_000;         // 67,304 B on the widest host: 696 B of
                                    // room. The browser's link barely moves
                                    // between hosts (~27 B) because wasm-ld
                                    // drops the bot brains from it entirely.
const WEB_GZ_MAX = 33 * 1024;       // 31,073 B today; 2,719 B of room, which is
                                    // 4.7x the 576 B compressor spread above
test('the kernel ships as small gzip static assets (not base64 embeds)', () => {
    for (const [rel, rawMax, gzMax] of [
        ['sdk/ts/wasm/bots.wasm.gz', BOTS_RAW_MAX, BOTS_GZ_MAX],
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

test('the browser downloads a fraction of the server-side kernel', () => {
    // The point of the split, asserted rather than described. Not a tight
    // ratio - it is here so that folding the server's surface back into the
    // browser's link (a wasm_table_* export added to WASM_WEB_NAMES "just to
    // try something") shows up as a failure rather than as a slower first paint
    // nobody attributes.
    const web = readFileSync(resolve('sdk/ts/wasm/web.wasm.gz')).length;
    const bots = readFileSync(resolve('sdk/ts/wasm/bots.wasm.gz')).length;
    assert.ok(web * 2 < bots,
        `web.wasm.gz is ${web} B against bots.wasm.gz's ${bots} B - the browser's link has\n`
        + 'stopped being a fraction of the server\'s. Something large was added to\n'
        + 'c/Makefile WASM_WEB_NAMES, or the server\'s link lost most of its own surface.');
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

test("bots.wasm declared INITIAL memory is 37 pages (static buffers, not the runtime TT)", () => {
    // bots.wasm can't be pinned — it bump-allocates a per-family transposition
    // table at runtime (see the flat-across-families test below). So we assert
    // the INITIAL declared memory only: the static (data + bss) footprint the
    // linker places below __heap_base. It is dominated by a handful of large,
    // deliberate static buffers (llvm-nm on the objects):
    //   - g_io               400 KiB  the WASM_IO_CAP=409600 log-import buffer
    //   - solve_ws / rs_play  272 KiB  each, the cordite solver working set (the
    //                                  CD_WASM_OVERLAY aliases them, so the pair
    //                                  is one region, not two)
    //   - g_scratch/g_moves   232 KiB  each, move enumeration at MAX_LEGAL_MOVES
    //   - g_game / g_rs_game  136 KiB  each, the resident + replay Game structs
    //   - msg_seal.scratch /  136 KiB  each, the FMSG seal + rebase scratch games
    //     msg_rebase.probe              (iMessage is deliberately linked here for
    //                                    the browser's base64 twin)
    // Sum ~= 36 pages (2.36 MiB). The SOLVER's hot working set still stays in L1
    // at runtime (32 KiB TT + book, docs/L1_SPEND_PLAN.md §6); this is the cold
    // static image, and the edge runtime maps it lazily. A regression UP from 36
    // (a new static buffer, or the stack creeping up) trips this. Read straight
    // from the shipped gz artifact.
    //
    // 37, RAISED DELIBERATELY (Phase 8's generated structs). This budget was 37
    // from 6d863b5 (the animation core moving to C), fell back to 36 at fccdb8f
    // when dropping replay format v9 and the retrodiction machinery freed ~9 KiB
    // of statics, and is 37 again now. What crossed the line: the kernel entry
    // points whose arguments and results used to be byte strings a host packed
    // itself now cross as C STRUCTS, and a struct a host reads and writes has to
    // live somewhere below __heap_base (e2e/no_ts_game_shape.test.ts). Sizes
    // straight from the generated modules' _SIZE constants:
    //
    //   ReplayExtras       8,744 B   the share link's names and per-move gaps
    //                               (8,192 of it gaps[MAX_LOGS]: a game cannot
    //                                have more moves than log records, so the
    //                                bound is the game's, not a new cap)
    //   RosterSpec + read  3,528 B   the TEST entries' table and trailer answer
    //                               (its slots are 2-4x the roster's budgets on
    //                                purpose, so trimming a name stays the
    //                                kernel's judgement - see roster.h)
    //   ReplayFrameIndex   1,032 B   where one chunk of replay frames lies
    //   MsgHeader            656 B   the FMSG envelope header
    //   ReplayError           12 B   why the last replay call refused
    //   ---------------------------
    //   ~13,016 B of new static against 2,592 B of room under 36 pages.
    //
    // Measured with llvm-nm on the objects this build links:
    //
    //   named statics   2,347,192 B   (35.82 pages, 337 symbols)
    //   shadow stack       22,528 B   (-Wl,-z,stack-size=22528, --stack-first)
    //   ------------------------------
    //   total           2,369,720 B   = 36.16 pages -> 37 declared
    //
    // which leaves 55,112 BYTES under the 37-page line - room for a struct or
    // two, not for another g_io. The alternative was to pay for them out of
    // g_io (400 KiB, the biggest static here), and it was REJECTED: the
    // Makefile sizes that buffer for ~3,072 raw log records so the kernel can
    // filter the whole session itself, and narrowing a measured design target to
    // dodge a page is how a buffer quietly stops meeting it. The runtime peak is
    // unaffected - it is the solver's bump-allocated transposition table, which
    // the flat-across-families test below pins.
    const wasm = new Uint8Array(gunzipSync(readFileSync(resolve('sdk/ts/wasm/bots.wasm.gz'))));
    const { min } = memLimits(wasm);
    assert.equal(min, 37, `bots.wasm initial memory is ${min} pages (${min * PAGE}B); expected 37: the deliberate static buffers (g_io 400 KiB, the cordite solver working set, the move enumerators, the resident and replay Games, the FMSG seal and rebase scratch games) plus the bridge's generated structs (ReplayExtras 8,744 B, RosterSpec 3,448 B, ReplayFrameIndex 1,032 B, MsgHeader 656 B). Going UP is a regression: 55,112 B of static room are left under the 37-page line, so look for a new buffer below __heap_base. Going DOWN means something was freed - lower this pin and take the page back.`);
});

test("web.wasm declared INITIAL memory is 26 pages - 11 fewer than the server's link", () => {
    // A LINKER FACT, not a second budget. wasm-ld keeps a static only if
    // something an export reaches refers to it, so dropping the bot bridge and
    // the C Table from the browser's export list drops their buffers with their
    // code: the cordite solver working set, the world-log slots, g_io's 400 KiB
    // log-import staging (the session log the belief bots filter - a browser
    // imports no logs) and the table's own scratch. 37 - 26 = 11 pages, 720,896 B
    // of linear memory a tab no longer reserves, on top of the 50,852 B of
    // download and the 124,192 B of module it no longer compiles.
    //
    // Pinned rather than merely reported for the reason the bots pin exists:
    // going UP means a browser call site pulled a server-sized buffer across,
    // which is the moment to ask whether the call belongs there at all. Going
    // DOWN is a win - re-pin it here and say what was freed.
    const wasm = new Uint8Array(gunzipSync(readFileSync(resolve('sdk/ts/wasm/web.wasm.gz'))));
    const { min } = memLimits(wasm);
    assert.equal(min, 26,
        `web.wasm initial memory is ${min} pages (${min * PAGE} B); expected 26. `
        + 'Up means the browser\'s link started keeping a static it used to drop - look at what '
        + 'was added to c/Makefile WASM_WEB_NAMES, because an export is a GC root and roots the '
        + 'data its code reads. Down means something was freed: lower this pin in the same commit.');
});

test('loading the bot kernel (read + gunzip + instantiate) fits a 64MB-old-space node', () => {
    // The old base64 embed needed ~300MB transient just to PARSE; the gz static
    // asset is read, inflated and instantiated at runtime. Do exactly that (the
    // real load cost) in a node capped far below the edge worker budget. Pure
    // built-ins, no tsx — so it measures the wasm cost, not transpile/interop.
    const gzPath = resolve('sdk/ts/wasm/bots.wasm.gz');
    execFileSync(process.execPath, [
        '--max-old-space-size=64',
        '-e',
        `const {readFileSync}=require('node:fs');const {gunzipSync}=require('node:zlib');`
        + `const w=new Uint8Array(gunzipSync(readFileSync(${JSON.stringify(gzPath)})));`
        + `new WebAssembly.Instance(new WebAssembly.Module(w),{});`,
    ], { stdio: 'pipe' });
});

test('bots.wasm memory is bounded and flat across all MC bot families', async () => {
    // Capture every wasm memory as modules instantiate. Node bills grown-but-
    // untouched pages as virtual memory (invisible in RSS), so the buffer
    // sizes — what the edge external budget is charged for — are the metric.
    const memories: WebAssembly.Memory[] = [];
    const RealInstance = WebAssembly.Instance;
    (WebAssembly as unknown as { Instance: unknown }).Instance = function (mod: WebAssembly.Module, imports?: WebAssembly.Imports) {
        const inst = new RealInstance(mod, imports);
        const m = (inst.exports as { memory?: unknown }).memory;
        if (m instanceof WebAssembly.Memory) memories.push(m);
        return inst;
    } as unknown as typeof WebAssembly.Instance;
    (WebAssembly.Instance as unknown as { prototype: unknown }).prototype = RealInstance.prototype;

    // The production bot path: a table of bots dealt from a seed and driven by
    // the kernel's bot cycle (table_bot_drive), exactly as the server's loop runs
    // it (e2e/helpers/bot_table.ts). Imported after the Instance hook, so the
    // table's bots.wasm instance is captured.
    const { playBotTable, seedBytes } = await import('../helpers/bot_table.ts');

    const wasmTotal = () => memories.reduce((a, m) => a + m.buffer.byteLength, 0);

    const playGame = (id: string, keys: string[]) => {
        const g = playBotTable(keys, seedBytes(keys.length, id.charCodeAt(id.length - 1)), { gameId: id });
        assert.ok(g.actions > 0 && g.fool >= 0, `${id} did not finish`);
    };

    // Game 1 warms the shared solver scratch. Uses the shipped MC ladder bots
    // (octogen, cordite) — semtex/fulminate were dropped from the wasm build, so
    // they would fall back to random and not exercise the solver.
    playGame('mem1', ['octogen', 'cordite']);
    const afterFirst = wasmTotal();
    assert.ok(memories.length > 0, 'the bots.wasm instance was captured');
    // 150MB external is the edge budget; rules+bots plus scratch must sit
    // far below it so JS buffers/fetch bodies have room.
    assert.ok(afterFirst <= 16 * 1048576,
        `wasm memory after one game is ${(afterFirst / 1048576) | 0}MB; budget regression`);

    // Every further family and game must reuse the SAME scratch: exactly flat.
    // firecracker (robusta MC + espresso rollout) and blackpowder (belief MC +
    // exact endgame solver) are the other shipped MC ladder bots — they run on
    // the SAME shared world/solver/rollout scratch, so exercising them here must
    // not grow the module by a byte. This is the guard that would have caught the
    // per-family malloc regression that first shipped them.
    playGame('mem2', ['cordite', 'octogen']);
    playGame('mem3', ['blackpowder', 'firecracker']);            // 2p: solver + handwritten rollout
    playGame('mem4', ['firecracker', 'blackpowder', 'firecracker']); // 3p: espresso rollout, no solver
    const afterAll = wasmTotal();
    assert.equal(afterAll, afterFirst,
        `wasm memory grew ${((afterAll - afterFirst) / 1048576) | 0}MB across families; per-family allocations are back`);
});
