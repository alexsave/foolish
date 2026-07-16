// Fulminate profiling probe. Plays seeded games with a chosen seat layout and,
// at EVERY fulminate decision, tallies how many opponent seats got each policy
// label (FUL_DBG) and the trump-rate samples (FUL_TR). Use it to measure the
// strong-field MISLABEL rate (any non-HANDWRITTEN label on a strong seat is a
// mislabel) without running a full A/B.
//
//   npx tsx offlinefun/localtest/fulminate_probe.ts <fillers> <pcs> <games>
//   npx tsx offlinefun/localtest/fulminate_probe.ts espresso 4,6 40
import { calculateLegalMoves, getBotStrategy } from '@api/common/bot_strategy.ts';
import { shouldBotActCore, executeBotMove } from '@api/common/pure_bot_actions.ts';
import { game_done } from '@api/common/common_utils.ts';
import { start_game } from '@api/common/game_lifecycle.ts';
import { GAME_STATUS, PLAYER_STATUS } from '@api/core/types.ts';

const POL_NAMES = ['HANDWRITTEN', 'ESPRESSO', 'RANDOM', 'SIMPLE', 'GREEDY', 'HUMAN', 'PASSIVE', 'AGGRO'];

const fillers = (process.argv[2] ?? 'espresso').split(',');
const pcs = (process.argv[3] ?? '4,6').split(',').map(Number);
const games = Number(process.argv[4] ?? 40);

let _seed = 777 >>> 0;
Math.random = () => { _seed = (_seed * 1664525 + 1013904223) % 4294967296; return _seed / 4294967296; };
const saved = console.log.bind(console);
console.log = () => {}; console.warn = () => {}; console.error = () => {};

const mkP = (s: string, i: number): any => ({ player_id: `b${i}`, name: `${s}${i}`,
    status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: s });
const mkG = (keys: string[]): any => ({ players: keys.map((s, i) => mkP(s, i)),
    deck: [], logs: [], id: 'l', name: 'l', status: GAME_STATUS.PLAYING, deck_length: 0, discard_pile_length: 0,
    flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
    good_timestamp: null, good_players: [] });

const seatKeys = (n: number): string[] => {
    const keys: string[] = ['fulminate', 'cordite'];
    for (let i = 2; i < n; i++) keys.push(fillers[(i - 2) % fillers.length]);
    return keys;
};

(async () => {
    for (const pc of pcs) {
        const dbg = new Array(POL_NAMES.length).fill(0);
        const tr: number[] = [];
        (globalThis as any).FUL_DBG = dbg;
        (globalThis as any).FUL_TR = tr;
        const keys = seatKeys(pc);
        for (let gi = 0; gi < games; gi++) {
            const game = mkG(keys); start_game(game);
            let iters = 0;
            while (game_done(game) === null && iters++ < 4000) {
                const elig: number[] = [];
                for (let i = 0; i < game.players.length; i++) if (shouldBotActCore(game, game.players[i], i)) elig.push(i);
                if (elig.length === 0) break;
                for (let i = elig.length - 1; i > 0; i--) { const j = Math.floor(Math.random() * (i + 1)); [elig[i], elig[j]] = [elig[j], elig[i]]; }
                let acted = false;
                for (const pi of elig) {
                    const pl = game.players[pi];
                    const lm = calculateLegalMoves(game, pl.player_id);
                    if (lm.length === 0) continue;
                    const st = getBotStrategy(pl.strategy_key);
                    const mv = await st.chooseMove(game, pl.player_id, lm);
                    if (executeBotMove(game, pl, mv)) { acted = true; break; }
                }
                if (!acted) break;
            }
        }
        const total = dbg.reduce((a: number, b: number) => a + b, 0);
        const nonHw = total - dbg[0];
        const trMean = tr.length ? tr.reduce((a, b) => a + b, 0) / tr.length : 0;
        saved(`pc=${pc} fillers=[${fillers}] games=${games} seatLabelEvents=${total} nonHANDWRITTEN=${nonHw} (${(100*nonHw/Math.max(1,total)).toFixed(2)}%)`);
        const parts: string[] = [];
        for (let i = 0; i < POL_NAMES.length; i++) if (dbg[i] > 0) parts.push(`${POL_NAMES[i]}=${dbg[i]}`);
        saved(`   labels: ${parts.join(' ')}`);
        saved(`   trump-rate samples: n=${tr.length} mean=${trMean.toFixed(3)} max=${tr.length?Math.max(...tr).toFixed(3):0}`);
    }
})();
