// LOCAL edge-runtime memory/CPU diagnostic. To use: cp this file to
// supabase/functions/memtest/index.ts, run `supabase functions serve`, then
// curl "http://127.0.0.1:54321/functions/v1/memtest?keys=semtex,octogen&maxmoves=8".
// Lives OUTSIDE supabase/functions so deploys can never ship it.
// LOCAL-ONLY diagnostic (never deploy): imports the full bot stack and plays
// bot-vs-bot games in-memory — no DB — to reproduce the production
// "Memory limit exceeded" kills under the real edge runtime via
// `supabase functions serve`. Query: ?keys=semtex,octogen&games=1
import { serve } from 'https://deno.land/std@0.168.0/http/server.ts';

serve(async (req: Request) => {
    const url = new URL(req.url);
    const keys = (url.searchParams.get('keys') ?? 'semtex,octogen').split(',');
    const games = Number(url.searchParams.get('games') ?? '1');
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
        types: '../_shared/core/types.ts',
        constants: '../_shared/core/constants.ts',
        engine: '../sdk/ts/wasm/engine.ts',
        common_utils: '../_shared/common/common_utils.ts',
        bots: '../sdk/ts/wasm/bots.ts',
        bot_strategy: '../_shared/common/bot_strategy.ts',
        pure_bot_actions: '../_shared/common/pure_bot_actions.ts',
    };
    console.log(`[memtest] start ${mem()}`);
    for (const s of steps) {
        const path = MODS[s];
        if (!path) continue;
        const ts = Date.now();
        await import(path);
        console.log(`[memtest] imported ${s} in ${Date.now() - ts}ms ${mem()}`);
    }
    const cu = await import('../_shared/common/common_utils.ts');
    // start_game split out of common_utils (client-bundle hygiene) — see
    // _shared/common/game_lifecycle.ts.
    const gl = await import('../_shared/common/game_lifecycle.ts');
    const pba = await import('../_shared/common/pure_bot_actions.ts');
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
    return new Response(JSON.stringify({ ok: true, out, ms: Date.now() - t0 }), {
        headers: { 'Content-Type': 'application/json' },
    });
});
