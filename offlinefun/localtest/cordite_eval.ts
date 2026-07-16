// Offline eval + decision-latency benchmark for the TS cordite port.
//
//   npx tsx offlinefun/localtest/cordite_eval.ts [strategy] [opp] [pcs] [games]
//   e.g. npx tsx offlinefun/localtest/cordite_eval.ts cordite_max espresso 4,8 20
//
// Reports per-PC mean finish position of the hero (seat 0) and the hero's
// decision-time distribution (mean / p95 / max ms) — the number that must
// stay under the ~2s production budget.

import { calculateLegalMoves } from '../../server/api/common/bot_strategy.ts';
import { shouldBotActCore, executeBotMove } from '../../server/api/common/pure_bot_actions.ts';
import { game_done } from '../../server/api/common/common_utils.ts';
import { start_game } from '../../server/api/common/game_lifecycle.ts';
import { getBotStrategy } from '../../server/api/common/bot_strategy.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS } from '../../server/api/core/types.ts';

// Seed Math.random globally so deals are deterministic run-to-run.
let _seed = 424242;
Math.random = () => {
    _seed = (_seed * 1664525 + 1013904223) % 4294967296;
    return _seed / 4294967296;
};

// Silence server logging.
const saved = console.log.bind(console);
console.log = () => {};
console.warn = () => {};
console.error = () => {};
const print = saved;

const heroStrat = process.argv[2] ?? 'cordite';
const oppStrat = process.argv[3] ?? 'espresso';
const pcs = (process.argv[4] ?? '2,4,6,8').split(',').map(Number);
const gamesPerPc = Number(process.argv[5] ?? 20);

const mkPlayer = (strategy: string, index: number): PrivatePlayer => ({
    player_id: `bot_${index}`,
    name: `${strategy} ${index}`,
    status: PLAYER_STATUS.READY,
    is_ai: true,
    hand: [],
    awaiting_attack: false,
    hand_length: 0,
    strategy_key: strategy,
});

const mkGame = (pc: number): Game => ({
    players: [mkPlayer(heroStrat, 0), ...Array.from({ length: pc - 1 }, (_, i) => mkPlayer(oppStrat, i + 1))],
    deck: [],
    logs: [],
    id: 'local',
    name: 'local',
    status: GAME_STATUS.PLAYING,
    deck_length: 0,
    discard_pile_length: 0,
    flipped: null,
    power_suit: 0,
    first_attacker: 0,
    defender: 0,
    table_battles: [],
    elimination_order: [],
    good_timestamp: null,
    good_players: [],
});

const playOne = async (pc: number, decisionMs: number[]): Promise<number> => {
    const game = mkGame(pc);
    start_game(game);
    let iters = 0;
    while (game_done(game) === null && iters++ < 4000) {
        const eligible: number[] = [];
        for (let i = 0; i < game.players.length; i++) {
            if (shouldBotActCore(game, game.players[i], i)) eligible.push(i);
        }
        if (eligible.length === 0) break;
        for (let i = eligible.length - 1; i > 0; i--) {
            const j = Math.floor(Math.random() * (i + 1));
            [eligible[i], eligible[j]] = [eligible[j], eligible[i]];
        }
        let acted = false;
        for (const pi of eligible) {
            const player = game.players[pi];
            const legalMoves = calculateLegalMoves(game, player.player_id);
            if (legalMoves.length === 0) continue;
            const strategy = getBotStrategy(player.strategy_key);
            const t0 = performance.now();
            const move = await strategy.chooseMove(game, player.player_id, legalMoves);
            const dt = performance.now() - t0;
            if (pi === 0) decisionMs.push(dt);
            if (executeBotMove(game, player, move)) { acted = true; break; }
        }
        if (!acted) break;
    }
    if (game_done(game) === null) return -1;
    const heroId = game.players[0].player_id;
    const pos = game.elimination_order.indexOf(heroId);
    return pos >= 0 ? pos + 1 : pc;
};

const pct = (arr: number[], p: number): number => {
    if (arr.length === 0) return 0;
    const sorted = arr.slice().sort((a, b) => a - b);
    return sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))];
};

(async () => {
    print(`hero=${heroStrat} opp=${oppStrat} games/pc=${gamesPerPc}`);
    print(`pc  mean_fp  baseline  win%   dec_mean  dec_p95  dec_max  (ms, hero)`);
    for (const pc of pcs) {
        const decisionMs: number[] = [];
        let fpSum = 0, wins = 0, valid = 0;
        const t0 = performance.now();
        for (let gi = 0; gi < gamesPerPc; gi++) {
            const fp = await playOne(pc, decisionMs);
            if (fp < 0) continue;
            fpSum += fp;
            if (fp === 1) wins++;
            valid++;
        }
        const dt = (performance.now() - t0) / 1000;
        const mean = valid ? fpSum / valid : 0;
        const dMean = decisionMs.length ? decisionMs.reduce((a, b) => a + b, 0) / decisionMs.length : 0;
        print(`${String(pc).padStart(2)}  ${mean.toFixed(3)}   ${(1 + (pc - 1) / 2).toFixed(2)}    ${(100 * wins / Math.max(1, valid)).toFixed(0)}%   ${dMean.toFixed(1)}     ${pct(decisionMs, 0.95).toFixed(1)}    ${Math.max(0, ...decisionMs).toFixed(1)}   [${valid} games, ${dt.toFixed(1)}s]`);
    }
})();
