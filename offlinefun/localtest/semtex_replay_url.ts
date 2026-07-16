// Play offline semtex-vs-cordite games and print a shareable replay URL
// (moves + names via the extras section) for the first game the hero WINS.
//
//   npx tsx offlinefun/localtest/semtex_replay_url.ts [pc] [maxGames] [seed0]
//
// The encoded game is verified with verifyRoundTrip before printing, so the
// URL is guaranteed to decode on the site.
import { calculateLegalMoves, getBotStrategy } from '@api/common/bot_strategy.ts';
import { shouldBotActCore, executeBotMove } from '@api/common/pure_bot_actions.ts';
import { game_done } from '@api/common/common_utils.ts';
import { start_game } from '@api/common/game_lifecycle.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS } from '@api/core/types.ts';
import { encodeReplay, verifyRoundTrip } from '@api/common/replay/encode.ts';
import { encodeExtras, joinReplayCode, moveTimesFromLogs } from '@api/common/replay/extras.ts';

let _seed = 424242;
Math.random = () => { _seed = (_seed * 1664525 + 1013904223) % 4294967296; return _seed / 4294967296; };
const realLog = console.log.bind(console);
console.log = () => {}; console.warn = () => {}; console.error = () => {};

const pc = Number(process.argv[2] ?? 4);
const maxGames = Number(process.argv[3] ?? 50);
const seed0 = Number(process.argv[4] ?? 777);

const heroName = '%SEMTEX';
const oppNames = ['%CORDITE 1', '%CORDITE 2', '%CORDITE 3', '%CORDITE 4',
    '%CORDITE 5', '%CORDITE 6', '%CORDITE 7'];

const mkPlayer = (strat: string, i: number): PrivatePlayer => ({
    player_id: `bot_${i}`, name: i === 0 ? heroName : oppNames[i - 1],
    status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false,
    hand_length: 0, strategy_key: strat });

const mkGame = (n: number): Game => ({
    players: [mkPlayer('semtex', 0),
        ...Array.from({ length: n - 1 }, (_, i) => mkPlayer('cordite', i + 1))],
    deck: [], logs: [], id: 'replay', name: 'replay', status: GAME_STATUS.PLAYING,
    deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
    first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
    good_timestamp: null, good_players: [] } as unknown as Game);

const playOne = async (seed: number): Promise<Game | null> => {
    _seed = seed;
    const g = mkGame(pc);
    start_game(g as never);
    // Stamp believable per-move times (the extras section encodes gaps).
    let t = Date.now() / 1000 - 3600;
    for (const l of g.logs) l.created_at = new Date(t * 1000).toISOString();
    let iters = 0;
    while (game_done(g as never) === null && iters++ < 4000) {
        const elig: number[] = [];
        for (let i = 0; i < g.players.length; i++)
            if (shouldBotActCore(g as never, g.players[i] as never, i)) elig.push(i);
        if (elig.length === 0) break;
        for (let i = elig.length - 1; i > 0; i--) {
            const j = Math.floor(Math.random() * (i + 1));
            [elig[i], elig[j]] = [elig[j], elig[i]];
        }
        let acted = false;
        for (const pi of elig) {
            const p = g.players[pi];
            const moves = calculateLegalMoves(g as never, p.player_id);
            if (!moves.length) continue;
            const strat = getBotStrategy(p.strategy_key!);
            const before = g.logs.length;
            const mv = await strat.chooseMove(g as never, p.player_id, moves);
            if (executeBotMove(g as never, p as never, mv)) {
                t += 0.8 + (_seed % 97) / 40;   // 0.8-3.2s human-ish gaps
                for (let li = before; li < g.logs.length; li++) {
                    g.logs[li].created_at = new Date(t * 1000).toISOString();
                }
                acted = true;
                break;
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
        const foolId = game_done(g as never);
        const heroOut = g.elimination_order.indexOf('bot_0');
        const heroPos = heroOut >= 0 ? heroOut + 1 : pc;
        if (heroPos !== 1) continue;   // want a semtex WIN

        const input = {
            playerIds: g.players.map(p => p.player_id),
            logs: g.logs,
            flipped: g.flipped,
        };
        const { encoded } = await verifyRoundTrip(input as never);
        const names = g.players.map(p => p.name!);
        const extras = encodeExtras(names, moveTimesFromLogs(g.logs as never));
        const full = joinReplayCode(encoded.base32, extras);
        realLog(`seed=${seed0 + gi * 7919} pc=${pc} semtex_finish=1 fool=${foolId}`);
        realLog(`moves=${g.logs.length} code_len=${full.length}`);
        realLog(`https://foolish.cards/${full}`);
        return;
    }
    realLog('no semtex win found in budget');
})();
