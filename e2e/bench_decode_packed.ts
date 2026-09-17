// Microbench: the web's decode of one packed game envelope, today's
// decodePackedGame (sdk/ts/wire/view.ts): the envelope header, the packed
// roster trailer, the masked view blob read in TypeScript
// (sdk/ts/wire/packed_read.ts) and the viewToGame materialization into a
// PersonalGame / PublicGame.
//
// It is the baseline for the "marshal / decode" gate in
// docs/C_GAME_SHAPE_MIGRATION.md 4.0: the C decode plus snapshot that replaces
// it must be no slower.
//
// The envelopes are real ones. A seeded game is dealt and played by the
// handwritten bot until the table holds a battle, then packedViewOf (the builder
// behind player_views, the create response and a realtime push) writes one
// envelope for seat 0 and one for a spectator. Cases: 2p, 4p and 8p player
// views, and the 4p spectator view.
//
//   TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/bench_decode_packed.ts
//   BENCH_ITERS=20000 BENCH_RUNS=9 BENCH_JSON=1 ...
//
// Prints ns/op per case: the median of BENCH_RUNS timed runs of BENCH_ITERS
// decodes each, after a warmup, plus the min and max run.

import { start_game } from '@api/common/game_lifecycle.ts';
import { game_done } from '@api/common/common_utils.ts';
import { Game, GAME_STATUS, PLAYER_STATUS, PrivatePlayer, StrategyKey } from '@api/core/types.ts';
import { shouldBotActCore, processBotAction } from '@api/common/pure_bot_actions.ts';
import { calculateLegalMoves } from '@api/common/bot_strategy.ts';
import { packedViewOf } from '@api/common/player_views.ts';
import { __setDealSeedOverride } from '@sdk/ts/wasm/engine.ts';
import { decodePackedGame } from '@sdk/ts/wire/view.ts';
import { seedBytes } from './helpers/seeded_game.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }
const out = (s: string) => process.stdout.write(`${s}\n`);

const ITERS = Number(process.env.BENCH_ITERS || 20000);
const RUNS = Number(process.env.BENCH_RUNS || 9);

// A pinned clock for viewToGame's good-timer math, so the decode does the same
// work every iteration.
const NOW = () => 1_760_000_000_000;

// Deal a seeded game and let the handwritten bot play until a battle is on the
// table after at least `minActions` actions: a mid-bout board with hands, a
// deck, a trump and a table, which is what a client decodes most of the time.
async function midBout(np: number, minActions: number): Promise<Game> {
    const game = {
        id: `bench${np}`, name: `Bench ${np}p`, status: GAME_STATUS.PLAYING,
        players: Array.from({ length: np }, (_, i): PrivatePlayer => ({
            player_id: `00000000-0000-4000-8000-${String(i).padStart(12, '0')}`,
            name: `Player ${i + 1}`, status: PLAYER_STATUS.READY, is_ai: true, hand: [],
            awaiting_attack: false, hand_length: 0, strategy_key: 'handwritten' as StrategyKey,
        })),
        deck: [], logs: [], deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
        first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
        good_timestamp: null, good_players: [], version: 42,
    } as unknown as Game;
    __setDealSeedOverride(seedBytes(np, 7));
    try {
        start_game(game);
        let actions = 0;
        while (game_done(game) === null && actions < 5000) {
            if (actions >= minActions && game.table_battles.length > 0) break;
            let acted = false;
            for (let i = 0; i < game.players.length && !acted; i++) {
                const p = game.players[i];
                if (!shouldBotActCore(game, p, i)) continue;
                if (calculateLegalMoves(game, p.player_id).length === 0) continue;
                acted = await processBotAction(game, p);
            }
            if (!acted) break;
            actions++;
        }
        if (game.table_battles.length === 0) throw new Error(`${np}p: no mid-bout board after ${actions} actions`);
    } finally {
        __setDealSeedOverride(null);
    }
    // The envelope names the viewer against the roster, so a human seat 0.
    game.players[0].is_ai = false;
    return game;
}

const median = (a: number[]) => { const s = [...a].sort((x, y) => x - y); const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };

function time(buf: Uint8Array): { median: number; min: number; max: number } {
    let sink = 0;
    const once = () => {
        for (let i = 0; i < ITERS; i++) {
            const d = decodePackedGame(buf, NOW);
            if (!d) throw new Error('envelope did not decode');
            sink += d.game.players.length;
        }
    };
    for (let w = 0; w < 3; w++) once();
    const perOp: number[] = [];
    for (let r = 0; r < RUNS; r++) {
        const t0 = process.hrtime.bigint();
        once();
        perOp.push(Number(process.hrtime.bigint() - t0) / ITERS);
    }
    if (sink === 0) throw new Error('unreachable');
    return { median: median(perOp), min: Math.min(...perOp), max: Math.max(...perOp) };
}

// tsx loads e2e files as CommonJS, which has no top-level await.
async function main(): Promise<void> {
    const games = { 2: await midBout(2, 6), 4: await midBout(4, 10), 8: await midBout(8, 14) };
    const cases: { name: string; buf: Uint8Array }[] = [
        { name: '2p player', buf: await packedViewOf(games[2], games[2].players[0].player_id) },
        { name: '4p player', buf: await packedViewOf(games[4], games[4].players[0].player_id) },
        { name: '8p player', buf: await packedViewOf(games[8], games[8].players[0].player_id) },
        { name: '4p spectator', buf: await packedViewOf(games[4], 'not-seated') },
    ];

    const results = cases.map(({ name, buf }) => {
        const d = decodePackedGame(buf, NOW)!;
        if (name.endsWith('spectator') ? d.seat !== -1 : d.seat !== 0) throw new Error(`${name}: decoded seat ${d.seat}`);
        return { name, bytes: buf.length, ...time(buf) };
    });

    if (process.env.BENCH_JSON) {
        out(JSON.stringify({ iters: ITERS, runs: RUNS, decodePackedGame: results }));
    } else {
        out(`decodePackedGame bench: ${ITERS} decodes x ${RUNS} runs per case (ns/op, median [min..max])`);
        for (const r of results) {
            out(`  ${r.name.padEnd(13)} ${String(r.bytes).padStart(4)} B  ${r.median.toFixed(0).padStart(7)} ns/op  [${r.min.toFixed(0)}..${r.max.toFixed(0)}]`);
        }
    }
}

main().catch((e) => { process.stderr.write(`${e?.stack ?? e}\n`); process.exit(1); });
