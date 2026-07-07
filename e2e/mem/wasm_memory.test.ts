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
import { resolve } from 'node:path';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

// bots.wasm now ships as a gzip STATIC ASSET (bots.wasm.gz) — a real binary, so
// concern #1 (parse-time concat blowup) is gone by construction for it. The
// rules kernel is still a base64 embed (rules_wasm.ts) because engine.ts is
// shared with the browser, which has no node:zlib — so it keeps the original
// single-line-literal guard.
test('bots.wasm ships as a small gzip static asset (not a base64 embed)', () => {
    const buf = readFileSync(resolve('supabase/functions/_shared/wasm/bots.wasm.gz'));
    assert.equal(buf[0], 0x1f, 'bots.wasm.gz is not gzip');
    assert.equal(buf[1], 0x8b, 'bots.wasm.gz is not gzip');
    assert.ok(buf.length < 80 * 1024, `bots.wasm.gz is ${(buf.length / 1024) | 0}KB; unexpectedly large`);
});

test('rules embed is a single-line literal (no parse-time concat garbage)', () => {
    const rel = 'supabase/functions/_shared/wasm/rules_wasm.ts';
    const src = readFileSync(resolve(rel), 'utf8');
    assert.ok(!/'\s*\+/.test(src), `${rel} is chunk-concatenated; regenerate with cnitro/wasm/embed.mjs`);
    assert.ok(src.trimEnd().split('\n').length <= 12, `${rel} is not a single-line embed`);
});

test('loading the bot kernel (read + gunzip + instantiate) fits a 64MB-old-space node', () => {
    // The old base64 embed needed ~300MB transient just to PARSE; the gz static
    // asset is read, inflated and instantiated at runtime. Do exactly that (the
    // real load cost) in a node capped far below the edge worker budget. Pure
    // built-ins, no tsx — so it measures the wasm cost, not transpile/interop.
    const gzPath = resolve('supabase/functions/_shared/wasm/bots.wasm.gz');
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

    const { game_done } = await import('../../supabase/functions/_shared/common_utils.ts');
    const { start_game } = await import('../../supabase/functions/_shared/game_lifecycle.ts');
    const { processBotAction, shouldBotActCore } = await import('../../supabase/functions/_shared/pure_bot_actions.ts');
    const { PLAYER_STATUS, GAME_STATUS } = await import('../../supabase/functions/_shared/types.ts');
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

    // Game 1 warms the shared solver scratch; the MC families it exercises
    // cover cordite-derived code paths (semtex, octogen).
    await playGame('mem1', ['semtex', 'octogen']);
    const afterFirst = wasmTotal();
    // 150MB external is the edge budget; rules+bots plus scratch must sit
    // far below it so JS buffers/fetch bodies have room.
    assert.ok(afterFirst <= 16 * 1048576,
        `wasm memory after one game is ${(afterFirst / 1048576) | 0}MB; budget regression`);

    // Every further family and game must reuse the SAME scratch: exactly flat.
    await playGame('mem2', ['cordite', 'semtex']);
    await playGame('mem3', ['fulminate', 'octogen']);
    const afterAll = wasmTotal();
    assert.equal(afterAll, afterFirst,
        `wasm memory grew ${((afterAll - afterFirst) / 1048576) | 0}MB across families; per-family allocations are back`);
});
