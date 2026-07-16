// Parallel strength + latency arena for the TS cordite port. Spawns N worker
// processes (default = CPU cores) that each play a slice of the games with a
// distinct base seed, then aggregates win-rate / mean-finish / p99 latency.
//
//   npx tsx offlinefun/localtest/cordite_arena.ts <hero> <opp> <pcs> <games> [workers]
//   CD_WORLDMUL=4 npx tsx offlinefun/localtest/cordite_arena.ts cordite handwritten 2,4,6,8 200
//
// Env knobs (CD_WORLDMUL, CD_NO_SOLVE, CD_NO_FASTROLL, CD_MAXMS) propagate to
// workers. Each worker is a child tsx process running this same file in
// --worker mode.

import { fork } from 'child_process';
import * as os from 'os';

const isWorker = process.argv.includes('--worker');

if (isWorker) {
    runWorker();
} else {
    runMaster();
}

interface Result { pc: number; n: number; wins: number; fpSum: number; dec: number[]; }

function runWorker(): void {
    // Lazy imports kept inside the worker branch so the master stays light.
    Promise.resolve().then(async () => {
        const { calculateLegalMoves } = await import('../../server/api/common/bot_strategy.ts');
        const { shouldBotActCore, executeBotMove } = await import('../../server/api/common/pure_bot_actions.ts');
        const { game_done } = await import('../../server/api/common/common_utils.ts');
        const { start_game } = await import('../../server/api/common/game_lifecycle.ts');
        const { getBotStrategy } = await import('../../server/api/common/bot_strategy.ts');
        const { GAME_STATUS, PLAYER_STATUS } = await import('../../server/api/core/types.ts');

        const hero = process.env.AR_HERO!, opp = process.env.AR_OPP!;
        const pcs = process.env.AR_PCS!.split(',').map(Number);
        const games = Number(process.env.AR_GAMES);
        const seed0 = Number(process.env.AR_SEED);

        if (process.env.CD_WORLDMUL) (globalThis as any).CD_WORLDMUL = Number(process.env.CD_WORLDMUL);
        if (process.env.CD_NO_SOLVE) (globalThis as any).CD_NO_SOLVE = true;
        if (process.env.SEMTEX_NO_PROFILE) (globalThis as any).SEMTEX_NO_PROFILE = true;
        if (process.env.SEMTEX_NO_ADAPT) (globalThis as any).SEMTEX_NO_ADAPT = true;
        if (process.env.CD_NO_FASTROLL) (globalThis as any).CD_NO_FASTROLL = true;
        const maxms = process.env.CD_MAXMS ? Number(process.env.CD_MAXMS) : 0;

        let _seed = seed0 >>> 0 || 1;
        Math.random = () => { _seed = (_seed * 1664525 + 1013904223) % 4294967296; return _seed / 4294967296; };
        console.log = () => {}; console.warn = () => {}; console.error = () => {};

        const mkP = (s: string, i: number): any => ({ player_id: `b${i}`, name: `${s}${i}`,
            status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: s });
        const mkG = (n: number): any => ({ players: [mkP(hero, 0), ...Array.from({ length: n - 1 }, (_, i) => mkP(opp, i + 1))],
            deck: [], logs: [], id: 'l', name: 'l', status: GAME_STATUS.PLAYING, deck_length: 0, discard_pile_length: 0,
            flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
            good_timestamp: null, good_players: [] });

        // Optionally override maxMillis on the strategy params (CD_MAXMS).
        if (maxms > 0) {
            const core = await import('./frozen/cordite_core.ts');
            (core.CORDITE_PARAMS as any).maxMillis = maxms;
            (core.CORDITE_MAX_PARAMS as any).maxMillis = maxms;
        }

        const results: Result[] = [];
        for (const pc of pcs) {
            const r: Result = { pc, n: 0, wins: 0, fpSum: 0, dec: [] };
            for (let gi = 0; gi < games; gi++) {
                const game = mkG(pc); start_game(game);
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
                        const t0 = performance.now();
                        const mv = await st.chooseMove(game, pl.player_id, lm);
                        const dt = performance.now() - t0;
                        if (pi === 0 && lm.length > 1) r.dec.push(dt);
                        if (executeBotMove(game, pl, mv)) { acted = true; break; }
                    }
                    if (!acted) break;
                }
                if (game_done(game) === null) continue;
                const pos = game.elimination_order.indexOf(game.players[0].player_id);
                const fp = pos >= 0 ? pos + 1 : pc;
                r.n++; r.fpSum += fp; if (fp === 1) r.wins++;
            }
            results.push(r);
        }
        process.send!({ results });
        process.exit(0);
    });
}

function pct(arr: number[], p: number): number {
    if (arr.length === 0) return 0;
    const s = arr.slice().sort((a, b) => a - b);
    return s[Math.min(s.length - 1, Math.floor(p * s.length))];
}

function runMaster(): void {
    const hero = process.argv[2] ?? 'cordite';
    const opp = process.argv[3] ?? 'handwritten';
    const pcs = process.argv[4] ?? '2,4,6,8';
    const totalGames = Number(process.argv[5] ?? 200);
    const workers = Number(process.argv[6] ?? Math.min(4, os.cpus().length));
    const per = Math.ceil(totalGames / workers);

    const agg = new Map<number, Result>();
    let done = 0;
    const t0 = Date.now();
    for (let w = 0; w < workers; w++) {
        const child = fork(__filename, ['--worker'], {
            env: { ...process.env, AR_HERO: hero, AR_OPP: opp, AR_PCS: pcs,
                AR_GAMES: String(per), AR_SEED: String(100003 + w * 7919) },
            execArgv: ['--import', 'tsx'],
        });
        child.on('message', (msg: any) => {
            for (const r of msg.results as Result[]) {
                let a = agg.get(r.pc);
                if (!a) { a = { pc: r.pc, n: 0, wins: 0, fpSum: 0, dec: [] }; agg.set(r.pc, a); }
                a.n += r.n; a.wins += r.wins; a.fpSum += r.fpSum;
                for (const d of r.dec) a.dec.push(d);
            }
        });
        child.on('exit', () => {
            if (++done === workers) report();
        });
    }

    function report(): void {
        const wall = (Date.now() - t0) / 1000;
        process.stdout.write(`hero=${hero} opp=${opp} games/pc=${[...agg.values()][0]?.n ?? 0} workers=${workers} WMUL=${process.env.CD_WORLDMUL ?? 1} MAXMS=${process.env.CD_MAXMS ?? 'def'} NO_SOLVE=${!!process.env.CD_NO_SOLVE} wall=${wall.toFixed(0)}s\n`);
        process.stdout.write(`pc  n    win%   mean_fp  base   dec_mean  dec_p95  dec_p99  dec_max  (ms)\n`);
        for (const pc of pcs.split(',').map(Number)) {
            const a = agg.get(pc); if (!a) continue;
            const win = 100 * a.wins / Math.max(1, a.n);
            const mean = a.fpSum / Math.max(1, a.n);
            const dMean = a.dec.length ? a.dec.reduce((x, y) => x + y, 0) / a.dec.length : 0;
            process.stdout.write(`${String(pc).padStart(2)}  ${String(a.n).padStart(3)}  ${win.toFixed(1).padStart(5)}  ${mean.toFixed(3)}   ${(1 + (pc - 1) / 2).toFixed(2)}  ${dMean.toFixed(1).padStart(7)}  ${pct(a.dec, 0.95).toFixed(0).padStart(6)}  ${pct(a.dec, 0.99).toFixed(0).padStart(6)}  ${Math.max(0, ...a.dec).toFixed(0).padStart(6)}\n`);
        }
    }
}
