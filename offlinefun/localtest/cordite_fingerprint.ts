// Deterministic outcome fingerprint: plays a fixed set of seeded games and
// prints the sequence of hero finish positions + a hash. Used to prove a
// refactor is behavior-identical (same fingerprint before/after).
import { calculateLegalMoves } from '../../supabase/functions/_shared/bot_strategy.ts';
import { shouldBotActCore, executeBotMove } from '../../supabase/functions/_shared/pure_bot_actions.ts';
import { game_done } from '../../supabase/functions/_shared/common_utils.ts';
import { start_game } from '../../supabase/functions/_shared/game_lifecycle.ts';
import { getBotStrategy } from '../../supabase/functions/_shared/bot_strategy.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS } from '../../supabase/functions/_shared/types.ts';

let _seed = 424242;
Math.random = () => { _seed = (_seed * 1664525 + 1013904223) % 4294967296; return _seed / 4294967296; };
const saved = console.log.bind(console);
console.log = () => {}; console.warn = () => {}; console.error = () => {};

if (process.env.CD_NO_FASTROLL) (globalThis as any).CD_NO_FASTROLL = true;
if (process.env.CD_NO_SOLVE) (globalThis as any).CD_NO_SOLVE = true;

const hero = process.argv[2] ?? 'cordite';
const opp = process.argv[3] ?? 'handwritten';
const pcs = (process.argv[4] ?? '2,4,6').split(',').map(Number);
const games = Number(process.argv[5] ?? 10);

const mkPlayer = (s: string, i: number): PrivatePlayer => ({
    player_id: `bot_${i}`, name: `${s} ${i}`, status: PLAYER_STATUS.READY,
    is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: s });
const mkGame = (n: number): Game => ({
    players: [mkPlayer(hero, 0), ...Array.from({ length: n - 1 }, (_, i) => mkPlayer(opp, i + 1))],
    deck: [], logs: [], id: 'l', name: 'l', status: GAME_STATUS.PLAYING, deck_length: 0,
    discard_pile_length: 0, flipped: null, power_suit: 0, first_attacker: 0, defender: 0,
    table_battles: [], elimination_order: [], good_timestamp: null, good_players: [] });

(async () => {
    const seq: number[] = [];
    for (const pc of pcs) {
        for (let gi = 0; gi < games; gi++) {
            const game = mkGame(pc); start_game(game);
            let iters = 0;
            while (game_done(game) === null && iters++ < 4000) {
                const elig: number[] = [];
                for (let i = 0; i < game.players.length; i++)
                    if (shouldBotActCore(game, game.players[i], i)) elig.push(i);
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
            const pos = game.elimination_order.indexOf(game.players[0].player_id);
            seq.push(pos >= 0 ? pos + 1 : pc);
        }
    }
    let h = 2166136261 >>> 0;
    for (const v of seq) { h ^= v; h = Math.imul(h, 16777619) >>> 0; }
    saved(`FP seq=[${seq.join(',')}] hash=${h}`);
})();
