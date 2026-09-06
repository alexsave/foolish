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

// bots.wasm now ships as a gzip STATIC ASSET (bots.wasm.gz) — a real binary, so
// concern #1 (parse-time concat blowup) is gone by construction for it. The
// rules kernel is still a base64 embed (rules_wasm.ts) because engine.ts is
// shared with the browser, which has no node:zlib — so it keeps the original
// single-line-literal guard.
test('bots.wasm ships as a small gzip static asset (not a base64 embed)', () => {
    const buf = readFileSync(resolve('sdk/ts/wasm/bots.wasm.gz'));
    assert.equal(buf[0], 0x1f, 'bots.wasm.gz is not gzip');
    assert.equal(buf[1], 0x8b, 'bots.wasm.gz is not gzip');
    assert.ok(buf.length < 80 * 1024, `bots.wasm.gz is ${(buf.length / 1024) | 0}KB; unexpectedly large`);
});

test('rules embed is a single-line literal (no parse-time concat garbage)', () => {
    const rel = 'sdk/ts/wasm/rules_wasm.ts';
    const src = readFileSync(resolve(rel), 'utf8');
    assert.ok(!/'\s*\+/.test(src), `${rel} is chunk-concatenated; regenerate with c/wasm/embed.mjs`);
    assert.ok(src.trimEnd().split('\n').length <= 12, `${rel} is not a single-line embed`);
});

// R0 pin + R1 arena overlay + R4 stack shrink
// (docs/RULES_GUARDS_WASM_MEMORY_PLAN.md): rules.wasm linear memory is now a
// hard-pinned 3 pages (was 5 pre-round: overlay -1 page, stack 64->32 KiB -1
// page). The pin (--initial-memory == --max-memory) means the module can never
// memory.grow, so the instance's memory is EXACTLY 3 pages and stays flat for
// the process's life. Read the committed embed straight off disk (base64 ->
// gunzip) so this runs in CI without a build, and without disturbing the
// take-once accessor the engine uses. guards.wasm's 1-page pin is asserted
// alongside for good measure.
const PAGE = 65536;
function embedWasmBytes(relTs: string): Uint8Array {
    const src = readFileSync(resolve(relTs), 'utf8');
    const m = src.match(/'([A-Za-z0-9+/=]+)'/);
    assert.ok(m, `${relTs}: no base64 literal found`);
    return new Uint8Array(gunzipSync(Buffer.from(m![1], 'base64')));
}
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

test('rules.wasm linear memory is pinned flat at 3 pages (R0 pin + R1 overlay + R4 stack)', () => {
    const wasm = embedWasmBytes('sdk/ts/wasm/rules_wasm.ts');
    const { min, max } = memLimits(wasm);
    assert.equal(min, 3, `rules.wasm initial memory is ${min} pages (${min * PAGE}B); expected 3 — a static buffer grew, or the overlay/stack regressed`);
    assert.equal(max, 3, `rules.wasm max memory is ${max} pages; expected a hard 3-page pin (--initial-memory == --max-memory)`);

    // Belt-and-braces: the module actually instantiates at exactly 3 pages and
    // cannot grow past the pin (a memory.grow would trap — but the linker also
    // strips any grow path, since rules.wasm has no allocator).
    const inst = new WebAssembly.Instance(new WebAssembly.Module(wasm), {});
    const mem = (inst.exports as { memory: WebAssembly.Memory }).memory;
    assert.equal(mem.buffer.byteLength, 3 * PAGE, 'rules.wasm did not instantiate at 3 pages');
    assert.throws(() => mem.grow(1), 'rules.wasm memory grew past its pin — the pin is not enforced');
    assert.equal(mem.buffer.byteLength, 3 * PAGE, 'rules.wasm memory changed after a rejected grow');
});

test('guards.wasm linear memory is pinned flat at 1 page', () => {
    // guards.wasm ships as a gzip embed like rules; assert its 1-page L1 pin is
    // intact (it shares game.c/view.c/awire.c with rules, so a shared-flag leak
    // that regrew it would show here).
    const wasm = embedWasmBytes('sdk/ts/wasm/guards_wasm.ts');
    const { min, max } = memLimits(wasm);
    assert.equal(min, 1, `guards.wasm initial memory is ${min} pages; expected the 1-page L1 pin`);
    assert.equal(max, 1, `guards.wasm max memory is ${max} pages; expected a hard 1-page pin`);
});

test("bots.wasm declared INITIAL memory is 36 pages (static buffers, not the runtime TT)", () => {
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
    // 36 AGAIN, AND BY 2.5 KiB. This budget was 37 from 6d863b5 (the animation
    // core moving to C) until fccdb8f, where dropping replay format v9 and the
    // retrodiction machinery freed ~9 KiB of statics and the image fell back
    // under the line. The standing TODO that asked for exactly this is
    // therefore retired. Measured with llvm-nm on the objects this build links:
    //
    //   named statics   2,334,176 B   (35.62 pages, 297 symbols)
    //   shadow stack       22,528 B   (-Wl,-z,stack-size=22528, --stack-first)
    //   ------------------------------
    //   total           2,356,704 B   = 35.96 pages -> 36 declared
    //
    // which leaves 2,592 BYTES under the 36-page line. That is the whole margin:
    // one new static buffer bigger than ~2.5 KiB below __heap_base puts the page
    // straight back, and this test is what will say so. If it has to go to 37
    // again, raise it deliberately here rather than quietly, the way it was
    // raised and then paid back the first time.
    const wasm = new Uint8Array(gunzipSync(readFileSync(resolve('sdk/ts/wasm/bots.wasm.gz'))));
    const { min } = memLimits(wasm);
    assert.equal(min, 36, `bots.wasm initial memory is ${min} pages (${min * PAGE}B); expected 36 (the deliberate static buffers — IO cap, solver working set, FMSG scratch games). Going UP is a regression: there are only ~2.5 KiB of static room under the 36-page line, so look for a new buffer below __heap_base.`);
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

    const { game_done } = await import('@api/common/common_utils.ts');
    const { start_game } = await import('@api/common/game_lifecycle.ts');
    const { processBotAction, shouldBotActCore } = await import('@api/common/pure_bot_actions.ts');
    const { PLAYER_STATUS, GAME_STATUS } = await import('@api/core/types.ts');
    type AnyGame = Parameters<typeof start_game>[0];

    const wasmTotal = () => memories.reduce((a, m) => a + m.buffer.byteLength, 0);

    const playGame = async (id: string, keys: string[]) => {
        const g = {
            players: keys.map((k, i) => ({
                player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true,
                hand: [], awaiting_attack: false, hand_length: 0, strategy_key: k,
            })),
            deck: [], logs: [], id, name: id, status: GAME_STATUS.PLAYING,
            deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
            first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
            good_timestamp: null, good_players: [],
        } as unknown as AnyGame;
        start_game(g);
        let guard = 0;
        while (game_done(g) === null && ++guard < 2000) {
            let acted = false;
            for (let i = 0; i < (g as { players: unknown[] }).players.length; i++) {
                const p = (g as { players: PrivateLike[] }).players[i];
                if (!shouldBotActCore(g, p as never, i)) continue;
                if (await processBotAction(g, p as never)) { acted = true; break; }
            }
            if (!acted) break;
        }
        assert.notEqual(game_done(g), null, `${id} did not finish`);
    };
    type PrivateLike = { player_id: string };

    // Game 1 warms the shared solver scratch. Uses the shipped MC ladder bots
    // (octogen, cordite) — semtex/fulminate were dropped from the wasm build, so
    // they would fall back to random and not exercise the solver.
    await playGame('mem1', ['octogen', 'cordite']);
    const afterFirst = wasmTotal();
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
    await playGame('mem2', ['cordite', 'octogen']);
    await playGame('mem3', ['blackpowder', 'firecracker']);            // 2p: solver + handwritten rollout
    await playGame('mem4', ['firecracker', 'blackpowder', 'firecracker']); // 3p: espresso rollout, no solver
    const afterAll = wasmTotal();
    assert.equal(afterAll, afterFirst,
        `wasm memory grew ${((afterAll - afterFirst) / 1048576) | 0}MB across families; per-family allocations are back`);
});
