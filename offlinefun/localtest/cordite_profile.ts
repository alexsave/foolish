// Coarse profiling harness for the TS cordite port. Plays full games and
// aggregates the CDPROF counters + a per-decision timer to surface the real
// hotspot breakdown and the solver share (toggle CD_NO_SOLVE).
//
//   npx tsx offlinefun/localtest/cordite_profile.ts [pc] [games] [strategy]
//   CD_NO_SOLVE=1 npx tsx offlinefun/localtest/cordite_profile.ts 2 30
//   CD_WORLDMUL=4 npx tsx offlinefun/localtest/cordite_profile.ts 4 12

import { calculateLegalMoves } from '@api/common/bot_strategy.ts';
import { shouldBotActCore, executeBotMove } from '@api/common/pure_bot_actions.ts';
import { game_done } from '@api/common/common_utils.ts';
import { start_game } from '@api/common/game_lifecycle.ts';
import { getBotStrategy } from '@api/common/bot_strategy.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS } from '@api/core/types.ts';
import { CDPROF, cdProfReset } from './frozen/cordite_core.ts';

let _seed = 424242;
Math.random = () => { _seed = (_seed * 1664525 + 1013904223) % 4294967296; return _seed / 4294967296; };

const saved = console.log.bind(console);
console.log = () => {}; console.warn = () => {}; console.error = () => {};
const print = saved;

// Allow env vars to drive the globalThis knobs (so CD_NO_SOLVE=1 works).
if (process.env.CD_NO_SOLVE) (globalThis as any).CD_NO_SOLVE = true;
if (process.env.CD_WORLDMUL) (globalThis as any).CD_WORLDMUL = Number(process.env.CD_WORLDMUL);
if (process.env.CD_NO_FASTROLL) (globalThis as any).CD_NO_FASTROLL = true;

const pc = Number(process.argv[2] ?? 4);
const games = Number(process.argv[3] ?? 12);
const heroStrat = process.argv[4] ?? 'cordite';
const oppStrat = process.argv[5] ?? 'handwritten';

const mkPlayer = (strategy: string, index: number): PrivatePlayer => ({
    player_id: `bot_${index}`, name: `${strategy} ${index}`, status: PLAYER_STATUS.READY,
    is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: strategy,
});
const mkGame = (n: number): Game => ({
    players: [mkPlayer(heroStrat, 0), ...Array.from({ length: n - 1 }, (_, i) => mkPlayer(oppStrat, i + 1))],
    deck: [], logs: [], id: 'local', name: 'local', status: GAME_STATUS.PLAYING,
    deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
    first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
    good_timestamp: null, good_players: [],
});

const pct = (arr: number[], p: number): number => {
    if (arr.length === 0) return 0;
    const s = arr.slice().sort((a, b) => a - b);
    return s[Math.min(s.length - 1, Math.floor(p * s.length))];
};

(async () => {
    const decisionMs: number[] = [];
    let decisions = 0;
    cdProfReset();
    const t0 = performance.now();
    for (let gi = 0; gi < games; gi++) {
        const game = mkGame(pc);
        start_game(game);
        let iters = 0;
        while (game_done(game) === null && iters++ < 4000) {
            const eligible: number[] = [];
            for (let i = 0; i < game.players.length; i++)
                if (shouldBotActCore(game, game.players[i], i)) eligible.push(i);
            if (eligible.length === 0) break;
            for (let i = eligible.length - 1; i > 0; i--) {
                const j = Math.floor(Math.random() * (i + 1));
                [eligible[i], eligible[j]] = [eligible[j], eligible[i]];
            }
            let acted = false;
            for (const pi of eligible) {
                const player = game.players[pi];
                const lm = calculateLegalMoves(game, player.player_id);
                if (lm.length === 0) continue;
                const strat = getBotStrategy(player.strategy_key);
                const ta = performance.now();
                const move = await strat.chooseMove(game, player.player_id, lm);
                const dt = performance.now() - ta;
                if (pi === 0 && lm.length > 1) { decisionMs.push(dt); decisions++; }
                if (executeBotMove(game, player, move)) { acted = true; break; }
            }
            if (!acted) break;
        }
    }
    const wall = (performance.now() - t0) / 1000;
    const p = CDPROF;
    const dMean = decisionMs.length ? decisionMs.reduce((a, b) => a + b, 0) / decisionMs.length : 0;
    const f = (n: number) => (n / 1e6).toFixed(2) + 'M';
    print(`hero=${heroStrat} opp=${oppStrat} pc=${pc} games=${games} NO_SOLVE=${!!process.env.CD_NO_SOLVE} WMUL=${process.env.CD_WORLDMUL ?? 1}`);
    print(`decisions(hero,>1 move)=${decisions}  wall=${wall.toFixed(1)}s`);
    print(`dec ms: mean=${dMean.toFixed(1)} p50=${pct(decisionMs,0.5).toFixed(1)} p95=${pct(decisionMs,0.95).toFixed(1)} p99=${pct(decisionMs,0.99).toFixed(1)} max=${Math.max(0,...decisionMs).toFixed(1)}`);
    print(`counts: cloneSim=${f(p.cloneSim)} sampleWorld=${f(p.sampleWorld)} simulate=${f(p.simulate)} simTurns=${f(p.simTurns)}`);
    print(`        applyMove=${f(p.applyMove)} calcLegal=${f(p.calcLegal)} solveNodes=${f(p.solveNodes)}`);
    print(`per-decision: cloneSim=${(p.cloneSim/Math.max(1,decisions)).toFixed(0)} sampleWorld=${(p.sampleWorld/Math.max(1,decisions)).toFixed(0)} simulate=${(p.simulate/Math.max(1,decisions)).toFixed(0)} solveNodes=${(p.solveNodes/Math.max(1,decisions)).toFixed(0)}`);
    const heap = process.memoryUsage();
    print(`rss=${(heap.rss/1e6).toFixed(0)}MB heapUsed=${(heap.heapUsed/1e6).toFixed(0)}MB`);
})();
