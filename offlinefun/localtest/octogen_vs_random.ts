// Play offline 1-octogen-vs-7-random games (seed-dealt, so the explainer can
// reconstruct) and print a shareable replay URL + the deal seed for the first
// game octogen WINS.
//
//   npx tsx offlinefun/localtest/octogen_vs_random.ts [pc] [maxGames] [seed0]
import { calculateLegalMoves, getBotStrategy } from '../../supabase/functions/_shared/common/bot_strategy.ts';
import { shouldBotActCore, executeBotMove } from '../../supabase/functions/_shared/common/pure_bot_actions.ts';
import { game_done } from '../../supabase/functions/_shared/common/common_utils.ts';
import { start_game } from '../../supabase/functions/_shared/common/game_lifecycle.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS } from '../../supabase/functions/_shared/core/types.ts';
import { verifyRoundTrip } from '../../supabase/functions/_shared/common/replay/encode.ts';
import { encodeExtras, joinReplayCode, moveTimesFromLogs } from '../../supabase/functions/_shared/common/replay/extras.ts';

let _seed = 424242;
Math.random = () => { _seed = (_seed * 1664525 + 1013904223) % 4294967296; return _seed / 4294967296; };
const realLog = console.log.bind(console);
console.log = () => {}; console.warn = () => {}; console.error = () => {};

const pc = Number(process.argv[2] ?? 8);
const maxGames = Number(process.argv[3] ?? 60);
const seed0 = Number(process.argv[4] ?? 12345);

const names = ['%OCTOGEN', ...Array.from({ length: pc - 1 }, (_, i) => `%RANDOM ${i + 1}`)];
const mkPlayer = (strat: string, i: number): PrivatePlayer => ({
    player_id: `bot_${i}`, name: names[i], status: PLAYER_STATUS.READY, is_ai: true,
    hand: [], awaiting_attack: false, hand_length: 0, strategy_key: strat });
const mkGame = (n: number): Game => ({
    players: [mkPlayer('octogen', 0), ...Array.from({ length: n - 1 }, (_, i) => mkPlayer('random', i + 1))],
    deck: [], logs: [], id: 'replay', name: 'replay', status: GAME_STATUS.PLAYING,
    deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
    first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
    good_timestamp: null, good_players: [] } as unknown as Game);

const playOne = async (seed: number): Promise<Game | null> => {
    _seed = seed;
    const g = mkGame(pc);
    start_game(g as never);
    let t = Date.now() / 1000 - 3600;
    for (const l of g.logs) l.created_at = new Date(t * 1000).toISOString();
    let iters = 0;
    while (game_done(g as never) === null && iters++ < 8000) {
        const elig: number[] = [];
        for (let i = 0; i < g.players.length; i++)
            if (shouldBotActCore(g as never, g.players[i] as never, i)) elig.push(i);
        if (elig.length === 0) break;
        for (let i = elig.length - 1; i > 0; i--) { const j = Math.floor(Math.random() * (i + 1)); [elig[i], elig[j]] = [elig[j], elig[i]]; }
        let acted = false;
        for (const pi of elig) {
            const p = g.players[pi];
            const moves = calculateLegalMoves(g as never, p.player_id);
            if (!moves.length) continue;
            const strat = getBotStrategy(p.strategy_key!);
            const before = g.logs.length;
            const mv = await strat.chooseMove(g as never, p.player_id, moves);
            if (executeBotMove(g as never, p as never, mv)) {
                t += 0.6 + (_seed % 53) / 30;
                for (let li = before; li < g.logs.length; li++) g.logs[li].created_at = new Date(t * 1000).toISOString();
                acted = true; break;
            }
        }
        if (!acted) break;
    }
    return game_done(g as never) !== null ? g : null;
};

(async () => {
    for (let gi = 0; gi < maxGames; gi++) {
        const g = await playOne(seed0 + gi * 7919);
        if (!g) continue;
        const heroOut = g.elimination_order.indexOf('bot_0');
        const heroPos = heroOut >= 0 ? heroOut + 1 : pc;   // not eliminated = fool = last
        if (heroPos !== 1) continue;   // want an octogen WIN (finished first)
        const input = { playerIds: g.players.map((p) => p.player_id), logs: g.logs, flipped: g.flipped };
        const { encoded } = await verifyRoundTrip(input as never);
        const extras = encodeExtras(g.players.map((p) => p.name!), moveTimesFromLogs(g.logs as never));
        const full = joinReplayCode(encoded.base32, extras);
        realLog(JSON.stringify({
            url: `WWW.FOOLISH.CARDS/${full}`,
            seed: g.game_seed,
            players: pc, moves: g.logs.length,
            elimination: g.elimination_order,
            heroFinish: heroPos,
        }));
        return;
    }
    realLog(JSON.stringify({ error: 'no octogen win found in budget' }));
})();
