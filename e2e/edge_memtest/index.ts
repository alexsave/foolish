// LOCAL edge-runtime memory/CPU diagnostic. To use: cp this file to
// server/impls/supabase/functions/memtest/index.ts (memory.yml does this), run
// `supabase --workdir server/impls functions serve`, then
// curl "http://127.0.0.1:54321/functions/v1/memtest?keys=cordite,octogen&maxmoves=8".
// Lives OUTSIDE the functions tree so deploys can never ship it.
// LOCAL-ONLY diagnostic (never deploy): loads the server's C Table
// (sdk/ts/table/server_table.ts over bots.wasm) and plays bot-vs-bot games in
// memory - no DB - through exactly the cycle the bot loop runs (load the row, set
// its deal seed, import the session log when a belief bot is about to choose,
// table_bot_drive, commit the products), to reproduce the production "Memory
// limit exceeded" kills under the real edge runtime via
// `supabase functions serve`. Query: ?keys=cordite,octogen&games=1&seed=1
//
// The deals are a function of `seed` (default 1), never of crypto: a memory
// kill this reproduces must reproduce again from the same URL, and the one
// entropic draw in the system is the live deal (scripts/check_determinism.mjs).
//
// A key this build cannot dispatch is a 400, never a game. The whole point of
// this diagnostic is to measure a NAMED bot under the edge budget, and an
// unknown key used to resolve to `random` and play 8 cheap moves that looked
// exactly as green as the real thing - for three keysets naming two bots that
// had been culled from the tree (issue #111). The table refuses such a key
// itself (TABLE_E_UNKNOWN_BRAIN at table_add_bot), and the response says which
// brain each seat's roster actually holds, so the caller can assert identity
// rather than liveness.
//
// Phase 8 (docs/C_GAME_SHAPE_MIGRATION.md) moved it off the TypeScript Game
// (start_game, processBotAction, game_done) and the TS bot registry.
import { serve } from 'https://deno.land/std@0.168.0/http/server.ts';

// Static side-effect imports of the modules this measures. REQUIRED: post-A10
// the stack lives in repo-root sdk, OUTSIDE the served functions/ tree.
// `functions serve` only stages modules reachable by STATIC import into its
// compile sandbox - it does NOT follow the literal `await import()` strings
// below - so without these the dynamic imports 404 with "Module not found"
// (production `functions deploy` bundles the dynamic graph, so it is unaffected).
// Keep this list in sync with MODS below.
import '../../../../../sdk/ts/gen/game_layout.bots.ts';
// server_table's entry in the list above, as a NAMED import: same staging
// effect, and the games below need the bindings.
import { serverTable, type ServerTable, type TableProducts } from '../../../../../sdk/ts/table/server_table.ts';
import * as L from '../../../../../sdk/ts/gen/game_layout.bots.ts';

const refusal = (what: string, rc: number) => new Error(`[memtest] ${what} refused (${rc})`);

/** A lobby of `keys` as bots, dealt from `seed` by table ops; the unknown keys when the table refuses any. */
function dealLobby(table: ServerTable, keys: string[], seed: Uint8Array): { products: TableProducts } | { unknown: string[] } {
    let rc = table.create('host', 'Host');
    if (rc !== L.TABLE_OK) throw refusal('create', rc);
    const unknown: string[] = [];
    keys.forEach((key, i) => {
        const r = table.addBot('host', `p${i}`, `P${i}`, key, seed);
        if (r === L.TABLE_E_UNKNOWN_BRAIN) unknown.push(key);
        else if (r !== L.TABLE_OK) throw refusal(`add bot ${key}`, r);
    });
    if (unknown.length > 0) return { unknown };
    if ((rc = table.leave('host', 'host')) !== L.TABLE_OK) throw refusal('leave', rc);
    if ((rc = table.ready('p0', seed)) !== L.TABLE_OK) throw refusal('ready', rc);
    const p = table.commit('memtest', 1, Date.now());
    if (typeof p === 'number') throw refusal('commit', p);
    return { products: p };
}

const concat = (a: Uint8Array, b: Uint8Array) => { const o = new Uint8Array(a.length + b.length); o.set(a); o.set(b, a.length); return o; };
const hex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');

/** Game `gi`'s 32-byte deal seed under run seed `base`: an LCG stream, so a URL replays its games. */
function dealSeed(base: number, gi: number): Uint8Array {
    let s = (Math.imul(base >>> 0, 2654435761) + gi) >>> 0;
    return Uint8Array.from({ length: 32 }, () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s >>> 24; });
}

serve(async (req: Request) => {
    const url = new URL(req.url);
    const keys = (url.searchParams.get('keys') ?? 'cordite,octogen').split(',');
    const games = Number(url.searchParams.get('games') ?? '1');
    const runSeed = Number(url.searchParams.get('seed') ?? '1');
    const t0 = Date.now();
    const out: Record<string, unknown>[] = [];

    const mem = () => {
        try {
            // deno-lint-ignore no-explicit-any
            const m = (Deno as any).memoryUsage?.();
            return m ? `heap=${(m.heapUsed / 1048576) | 0}/${(m.heapTotal / 1048576) | 0}MB ext=${(m.external / 1048576) | 0}MB` : 'n/a';
        } catch { return 'n/a'; }
    };
    const steps = (url.searchParams.get('steps') ?? 'game_layout,server_table').split(',');
    const MODS: Record<string, string> = {
        game_layout: '../../../../../sdk/ts/gen/game_layout.bots.ts',
        server_table: '../../../../../sdk/ts/table/server_table.ts',
    };
    console.log(`[memtest] start ${mem()}`);
    for (const s of steps) {
        const path = MODS[s];
        if (!path) continue;
        const ts = Date.now();
        await import(path);
        console.log(`[memtest] imported ${s} in ${Date.now() - ts}ms ${mem()}`);
    }
    const table = await serverTable();
    console.log(`[memtest] table ready in ${Date.now() - t0}ms ${mem()} wasm=${(table.memoryBytes() / 1048576) | 0}MB`);

    // Identity, before anything is measured: the table refuses a key this build
    // does not link, and a refused key is the caller's bug, answered with a 400.
    const seed = dealSeed(runSeed, 0);
    const probe = dealLobby(table, keys, seed);
    if ('unknown' in probe) {
        console.log(`[memtest] unknown bot key(s): ${probe.unknown.join(', ')}`);
        return new Response(JSON.stringify({ ok: false, error: 'unknown bot key', unknown: probe.unknown }),
            { status: 400, headers: { 'Content-Type': 'application/json' } });
    }
    // The brain each seat's roster holds - what table_bot_drive dispatches.
    const seats = table.seats().map((s, seat) => ({ seat, key: keys[seat], strategy: s.brain }));

    const maxMoves = Number(url.searchParams.get('maxmoves') ?? '1000000');
    for (let gi = 0; gi < games; gi++) {
        const gameSeed = gi === 0 ? seed : dealSeed(runSeed, gi);
        const dealt = gi === 0 ? probe : dealLobby(table, keys, gameSeed);
        if (!('products' in dealt)) throw new Error('[memtest] the lobby changed between games');
        let row = { state: dealt.products.state, roster: dealt.products.roster, log: dealt.products.logs ?? new Uint8Array(0), version: 1 };
        const seedHex = hex(gameSeed);
        let moves = 0, done = false;
        for (let guard = 0; guard < 2000 && moves < maxMoves && !done; guard++) {
            let rc = table.load(row.state, row.roster);
            if (rc < 0) throw refusal('load', rc);
            if ((rc = table.setDealSeed(seedHex)) < 0) throw refusal('deal seed', rc);
            if (table.botsNeedLogs() && row.log.length > 0 && (rc = table.importSessionLog(row.log)) < 0) throw refusal('session log', rc);
            if (moves < 6) console.log(`[memtest] before cycle at move ${moves} ${mem()}`);
            const d = table.botDrive(null);
            if (typeof d === 'number') throw refusal('bot drive', d);
            if (d.n === 0) break;
            const p = table.commit(`memtest${gi}`, row.version + 1, Date.now());
            if (typeof p === 'number') throw refusal('commit', p);
            if (moves < 6) console.log(`[memtest] after  cycle of ${d.n} action(s) by seats [${d.seats.join(', ')}] ${mem()}`);
            row = { state: p.state, roster: p.roster, version: row.version + 1, log: p.logs === null ? row.log : p.logsReset ? p.logs : concat(row.log, p.logs) };
            moves += d.n;
            done = p.ended;
        }
        out.push({ game: gi, done, moves });
        console.log(`[memtest] game ${gi} (${keys.join(' vs ')}): done=${done} moves=${moves} elapsed=${Date.now() - t0}ms wasm=${(table.memoryBytes() / 1048576) | 0}MB`);
    }
    // `seats` is the identity half of the report: the brain each seat was
    // actually dispatched under, not the key the caller typed. A gate that
    // greps for its own bot names in here cannot be satisfied by `random`.
    return new Response(JSON.stringify({ ok: true, seats, out, ms: Date.now() - t0 }), {
        headers: { 'Content-Type': 'application/json' },
    });
});
