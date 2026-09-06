// LOCAL edge-runtime memory/CPU diagnostic. To use: cp this file to
// server/impls/supabase/functions/memtest/index.ts (memory.yml does this), run
// `supabase --workdir server/impls functions serve`, then
// curl "http://127.0.0.1:54321/functions/v1/memtest?keys=cordite,octogen&maxmoves=8".
// Lives OUTSIDE the functions tree so deploys can never ship it.
// LOCAL-ONLY diagnostic (never deploy): imports the full bot stack and plays
// bot-vs-bot games in-memory — no DB — to reproduce the production
// "Memory limit exceeded" kills under the real edge runtime via
// `supabase functions serve`. Query: ?keys=cordite,octogen&games=1
//
// A key this build cannot dispatch is a 400, never a game. The whole point of
// this diagnostic is to measure a NAMED bot under the edge budget, and an
// unknown key used to resolve to `random` (getBotStrategy's fallback) and play
// 8 cheap moves that looked exactly as green as the real thing — for three
// keysets naming two bots that had been culled from the tree (issue #111). The
// response now says which strategy each seat actually ran, so the caller can
// assert identity rather than liveness.
import { serve } from 'https://deno.land/std@0.168.0/http/server.ts';

// Static side-effect imports of the whole bot stack. REQUIRED: post-A10 the
// stack lives in server/api + repo-root sdk, OUTSIDE the served functions/ tree.
// `functions serve` only stages modules reachable by STATIC import into its
// compile sandbox — it does NOT follow the literal `await import()` strings
// below — so without these the dynamic imports 404 with "Module not found"
// (production `functions deploy` bundles the dynamic graph, so it is unaffected).
// These pull server/api + sdk into the sandbox; the dynamic import()s then
// resolve against the bundled files. Keep this list in sync with MODS below.
import '../../../../api/core/types.ts';
import '../../../../api/core/constants.ts';
import '../../../../../sdk/ts/wasm/engine.ts';
import '../../../../api/common/common_utils.ts';
import '../../../../../sdk/ts/wasm/bots.ts';
import '../../../../api/common/pure_bot_actions.ts';
import '../../../../api/common/game_lifecycle.ts';
// bot_strategy's entry in the list above, as a NAMED import: same staging
// effect, and the key -> strategy resolution below needs the bindings.
import { botStrategyKeys, resolveBotStrategy } from '../../../../api/common/bot_strategy.ts';

serve(async (req: Request) => {
    const url = new URL(req.url);
    const keys = (url.searchParams.get('keys') ?? 'cordite,octogen').split(',');
    const games = Number(url.searchParams.get('games') ?? '1');

    // Identity, before anything is measured. Resolve every key STRICTLY: an
    // unknown one is the caller's bug, and answering it with a `random` game
    // would report a bot that does not exist as healthy.
    const seats = keys.map((key, seat) => ({ seat, key, strategy: resolveBotStrategy(key)?.name ?? null }));
    const unknown = seats.filter((s) => s.strategy === null).map((s) => s.key);
    if (unknown.length > 0) {
        console.log(`[memtest] unknown bot key(s): ${unknown.join(', ')}`);
        return new Response(JSON.stringify({
            ok: false,
            error: 'unknown bot key',
            unknown,
            known: botStrategyKeys(),
        }), { status: 400, headers: { 'Content-Type': 'application/json' } });
    }
    const t0 = Date.now();
    const out: Record<string, unknown>[] = [];

    const mem = () => {
        try {
            // deno-lint-ignore no-explicit-any
            const m = (Deno as any).memoryUsage?.();
            return m ? `heap=${(m.heapUsed / 1048576) | 0}/${(m.heapTotal / 1048576) | 0}MB ext=${(m.external / 1048576) | 0}MB` : 'n/a';
        } catch { return 'n/a'; }
    };
    const steps = (url.searchParams.get('steps') ?? 'types,constants,engine,common_utils,bots,bot_strategy,pure_bot_actions').split(',');
    const MODS: Record<string, string> = {
        types: '../../../../api/core/types.ts',
        constants: '../../../../api/core/constants.ts',
        engine: '../../../../../sdk/ts/wasm/engine.ts',
        common_utils: '../../../../api/common/common_utils.ts',
        bots: '../../../../../sdk/ts/wasm/bots.ts',
        bot_strategy: '../../../../api/common/bot_strategy.ts',
        pure_bot_actions: '../../../../api/common/pure_bot_actions.ts',
    };
    console.log(`[memtest] start ${mem()}`);
    for (const s of steps) {
        const path = MODS[s];
        if (!path) continue;
        const ts = Date.now();
        await import(path);
        console.log(`[memtest] imported ${s} in ${Date.now() - ts}ms ${mem()}`);
    }
    const cu = await import('../../../../api/common/common_utils.ts');
    // start_game split out of common_utils (client-bundle hygiene) — see
    // _shared/common/game_lifecycle.ts.
    const gl = await import('../../../../api/common/game_lifecycle.ts');
    const pba = await import('../../../../api/common/pure_bot_actions.ts');
    console.log(`[memtest] import done in ${Date.now() - t0}ms ${mem()}`);

    for (let gi = 0; gi < games; gi++) {
        const g = {
            players: keys.map((k, i) => ({
                player_id: `p${i}`, name: `P${i}`, status: 'ready', is_ai: true,
                hand: [], awaiting_attack: false, hand_length: 0, strategy_key: k,
            })),
            deck: [], logs: [], id: `memtest${gi}`, name: 'memtest', status: 'playing',
            deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
            first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
            good_timestamp: null, good_players: [],
        };
        // deno-lint-ignore no-explicit-any
        gl.start_game(g as any);
        const maxMoves = Number(url.searchParams.get('maxmoves') ?? '1000000');
        let guard = 0, moves = 0;
        // deno-lint-ignore no-explicit-any
        while (cu.game_done(g as any) === null && ++guard < 2000 && moves < maxMoves) {
            let acted = false;
            for (let i = 0; i < g.players.length; i++) {
                // deno-lint-ignore no-explicit-any
                if (!pba.shouldBotActCore(g as any, g.players[i] as any, i)) continue;
                if (moves < 6) console.log(`[memtest] before move ${moves} (${g.players[i].strategy_key}) ${mem()}`);
                // deno-lint-ignore no-explicit-any
                const r = await pba.processBotAction(g as any, g.players[i] as any);
                if (moves < 6) console.log(`[memtest] after  move ${moves} ${mem()}`);
                if (r) { moves++; acted = true; break; }
            }
            if (!acted) break;
        }
        // deno-lint-ignore no-explicit-any
        const done = cu.game_done(g as any) !== null;
        out.push({ game: gi, done, moves });
        console.log(`[memtest] game ${gi} (${keys.join(' vs ')}): done=${done} moves=${moves} elapsed=${Date.now() - t0}ms`);
    }
    // `seats` is the identity half of the report: the strategy each seat was
    // actually dispatched under, not the key the caller typed. A gate that
    // greps for its own bot names in here cannot be satisfied by `random`.
    return new Response(JSON.stringify({ ok: true, seats, out, ms: Date.now() - t0 }), {
        headers: { 'Content-Type': 'application/json' },
    });
});
