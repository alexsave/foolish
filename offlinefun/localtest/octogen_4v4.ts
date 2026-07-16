// Play offline 4-octogen (seats 0-3) vs 4-random (seats 4-7) games, seed-dealt
// so the explainer can reconstruct exactly, and print a shareable replay URL +
// the deal seed for the first game an octogen WINS (finishes first).
//
//   npx tsx offlinefun/localtest/octogen_4v4.ts [maxGames] [seed0]
import { calculateLegalMoves, getBotStrategy } from '../../server/api/common/bot_strategy.ts';
import { shouldBotActCore, executeBotMove } from '../../server/api/common/pure_bot_actions.ts';
import { game_done } from '../../server/api/common/common_utils.ts';
import { start_game } from '../../server/api/common/game_lifecycle.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS } from '../../server/api/core/types.ts';
import { verifyRoundTrip } from '../../server/api/common/replay/encode.ts';
import { encodeExtras, joinReplayCode, moveTimesFromLogs } from '../../server/api/common/replay/extras.ts';

let _seed = 424242;
Math.random = () => { _seed = (_seed * 1664525 + 1013904223) % 4294967296; return _seed / 4294967296; };
const realLog = console.log.bind(console);
console.log = () => {}; console.warn = () => {}; console.error = () => {};

const PC = 8;
const OCTO = new Set([0, 1, 2, 3]);   // seats 0-3 octogen, 4-7 random
const maxGames = Number(process.argv[2] ?? 80);
const seed0 = Number(process.argv[3] ?? 12345);

const names = Array.from({ length: PC }, (_, i) =>
    OCTO.has(i) ? `%OCTOGEN ${i + 1}` : `%RANDOM ${i - 3}`);
const mkPlayer = (i: number): PrivatePlayer => ({
    player_id: `bot_${i}`, name: names[i], status: PLAYER_STATUS.READY, is_ai: true,
    hand: [], awaiting_attack: false, hand_length: 0,
    strategy_key: OCTO.has(i) ? 'octogen' : 'random' });
const mkGame = (): Game => ({
    players: Array.from({ length: PC }, (_, i) => mkPlayer(i)),
    deck: [], logs: [], id: 'replay', name: 'replay', status: GAME_STATUS.PLAYING,
    deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
    first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
    good_timestamp: null, good_players: [] } as unknown as Game);

const playOne = async (seed: number): Promise<Game | null> => {
    _seed = seed;
    const g = mkGame();
    start_game(g as never);
    let t = Date.now() / 1000 - 3600;
    for (const l of g.logs) l.created_at = new Date(t * 1000).toISOString();
    let iters = 0;
    while (game_done(g as never) === null && iters++ < 12000) {
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
        // Rank: index in elimination_order (earlier = better). Not eliminated = fool = last.
        const finishOf = (id: string) => {
            const k = g.elimination_order.indexOf(id);
            return k >= 0 ? k + 1 : PC;
        };
        const winnerId = g.elimination_order[0];
        const winnerSeat = g.players.findIndex((p) => p.player_id === winnerId);
        if (!OCTO.has(winnerSeat)) continue;   // want an octogen to win
        const input = { playerIds: g.players.map((p) => p.player_id), logs: g.logs, flipped: g.flipped };
        const { encoded } = await verifyRoundTrip(input as never);
        const extras = encodeExtras(g.players.map((p) => p.name!), moveTimesFromLogs(g.logs as never));
        const full = joinReplayCode(encoded.base32, extras);
        realLog(JSON.stringify({
            url: `WWW.FOOLISH.CARDS/${full}`,
            seed: g.game_seed,
            players: PC,
            octogenSeats: [...OCTO],
            moves: g.logs.length,
            elimination: g.elimination_order,
            finishBySeat: g.players.map((p) => finishOf(p.player_id)),
            fool: g.players.findIndex((p) => !g.elimination_order.includes(p.player_id)),
        }));
        return;
    }
    realLog(JSON.stringify({ error: 'no octogen win found in budget' }));
})();
