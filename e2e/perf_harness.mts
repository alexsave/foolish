// Ad-hoc harness (not part of the suite): bot decision latency through the
// production wasm path, with pinned RNG so runs are comparable across builds.
// Reports per-family decision timing over full games.
import { start_game, game_done } from '../supabase/functions/_shared/common_utils.ts';
import { processBotAction, shouldBotActCore } from '../supabase/functions/_shared/pure_bot_actions.ts';
import { __setBotSeedSource } from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { __setKernelSeedSource } from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';
import { Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS } from '../supabase/functions/_shared/types.ts';

const mkLcgU32 = (seed: number) => {
    let s = seed >>> 0;
    return () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s; };
};

const mkPlayer = (i: number, key: string): PrivatePlayer => ({
    player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true,
    hand: [], awaiting_attack: false, hand_length: 0,
    strategy_key: key as PrivatePlayer['strategy_key'],
});

const mkGame = (id: string, keys: string[]): Game => ({
    players: keys.map((k, i) => mkPlayer(i, k)),
    deck: [], logs: [], id, name: id, status: GAME_STATUS.PLAYING,
    deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
    first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
    good_timestamp: null, good_players: [],
});

if (!process.env.E2E_VERBOSE) { console.log = () => {}; }
const report = console.error.bind(console); // survives the console.log gag

const GAMES = Number(process.env.PERF_GAMES ?? '6');
const stats: Record<string, { n: number; ms: number; max: number }> = {};

for (const keys of [['semtex', 'octogen'], ['cordite', 'fulminate']]) {
    for (let gi = 0; gi < GAMES; gi++) {
        __setKernelSeedSource(mkLcgU32(0xDEA1 ^ gi));
        const meta = mkLcgU32(0xB07 ^ gi);
        __setBotSeedSource(() => meta());
        const g = mkGame(`perf${gi}`, keys);
        start_game(g);
        let guard = 0;
        while (game_done(g) === null && ++guard < 2000) {
            let acted = false;
            for (let i = 0; i < g.players.length; i++) {
                const p = g.players[i];
                if (!shouldBotActCore(g, p, i)) continue;
                const t = performance.now();
                const r = await processBotAction(g, p);
                const dt = performance.now() - t;
                const s = (stats[p.strategy_key] ??= { n: 0, ms: 0, max: 0 });
                s.n++; s.ms += dt; if (dt > s.max) s.max = dt;
                if (r) { acted = true; break; }
            }
            if (!acted) break;
        }
    }
}

for (const [k, s] of Object.entries(stats)) {
    report(`${k.padEnd(10)} decisions=${s.n} avg=${(s.ms / s.n).toFixed(1)}ms max=${s.max.toFixed(0)}ms total=${(s.ms / 1000).toFixed(1)}s`);
}
