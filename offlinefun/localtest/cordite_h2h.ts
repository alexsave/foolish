// Head-to-head: the NEW cordite (production core) vs the OLD cordite
// (frozen pre-change snapshot, offlinefun/localtest/frozen). Seat 0 is the
// NEW bot; the other (n-1) seats are the OLD bot. If NEW is genuinely
// stronger it finishes 1st more than its fair share (1/n) and carries a
// lower mean finish position than the symmetric baseline (1+(n-1)/2).
//
//   npx tsx offlinefun/localtest/cordite_h2h.ts <pcs> <games> [workers]
//   CD_MAXMS=2000 npx tsx offlinefun/localtest/cordite_h2h.ts 2,4,6 400
//
// Env knobs (CD_WORLDMUL, CD_NO_SOLVE, CD_NO_FASTROLL, CD_MAXMS) affect BOTH
// cordite cores in-process, so leave them unset for the deployment-faithful
// comparison; they exist only for ablation. The OLD core is fixed regardless.

import { fork } from 'child_process';
import * as os from 'os';

const isWorker = process.argv.includes('--worker');
if (isWorker) runWorker(); else runMaster();

interface Result { pc: number; n: number; wins: number; fpSum: number; dec: number[]; }

function runWorker(): void {
    Promise.resolve().then(async () => {
        const { calculateLegalMoves, getBotStrategy, registerBotStrategy } =
            await import('@api/common/bot_strategy.ts');
        const { shouldBotActCore, executeBotMove } = await import('@api/common/pure_bot_actions.ts');
        const { game_done } = await import('@api/common/common_utils.ts');
        const { start_game } = await import('@api/common/game_lifecycle.ts');
        const { GAME_STATUS, PLAYER_STATUS } = await import('@api/core/types.ts');
        const { CorditeOldStrategy } = await import('./frozen/cordite_old_strategy.ts');
        registerBotStrategy('cordite_old', new CorditeOldStrategy());

        const pcs = process.env.AR_PCS!.split(',').map(Number);
        const games = Number(process.env.AR_GAMES);
        const seed0 = Number(process.env.AR_SEED);
        // The core reads these off globalThis (offline ablation knobs); env vars
        // alone are invisible to it, so mirror them in. The frozen OLD core
        // predates these knobs and ignores them, so e.g. CD_WORLDMUL scales ONLY
        // the NEW (seat-0) bot — a clean "does more compute help NEW?" probe.
        if (process.env.CD_WORLDMUL) (globalThis as any).CD_WORLDMUL = Number(process.env.CD_WORLDMUL);
        if (process.env.CD_NO_SOLVE) (globalThis as any).CD_NO_SOLVE = true;
        if (process.env.CD_NO_FASTROLL) (globalThis as any).CD_NO_FASTROLL = true;
        if (process.env.CD_MAXMS) {
            const core = await import('./frozen/cordite_core.ts');
            (core.CORDITE_PARAMS as any).maxMillis = Number(process.env.CD_MAXMS);
            (core.CORDITE_MAX_PARAMS as any).maxMillis = Number(process.env.CD_MAXMS);
        }

        let _seed = seed0 >>> 0 || 1;
        Math.random = () => { _seed = (_seed * 1664525 + 1013904223) % 4294967296; return _seed / 4294967296; };
        console.log = () => {}; console.warn = () => {}; console.error = () => {};

        const mkP = (s: string, i: number): any => ({ player_id: `b${i}`, name: `${s}${i}`,
            status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: s });
        const mkG = (n: number): any => ({ players: [mkP('cordite', 0), ...Array.from({ length: n - 1 }, (_, i) => mkP('cordite_old', i + 1))],
            deck: [], logs: [], id: 'l', name: 'l', status: GAME_STATUS.PLAYING, deck_length: 0, discard_pile_length: 0,
            flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
            good_timestamp: null, good_players: [] });

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
    const pcs = process.argv[2] ?? '2,4,6';
    const totalGames = Number(process.argv[3] ?? 400);
    const workers = Number(process.argv[4] ?? Math.min(4, os.cpus().length));
    const per = Math.ceil(totalGames / workers);
    const agg = new Map<number, Result>();
    let done = 0; const t0 = Date.now();
    for (let w = 0; w < workers; w++) {
        const child = fork(__filename, ['--worker'], {
            env: { ...process.env, AR_PCS: pcs, AR_GAMES: String(per), AR_SEED: String(100003 + w * 7919) },
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
        child.on('exit', () => { if (++done === workers) report(); });
    }
    function report(): void {
        const wall = (Date.now() - t0) / 1000;
        process.stdout.write(`NEW(seat0) vs OLD(rest)  games/pc=${[...agg.values()][0]?.n ?? 0} workers=${workers} MAXMS=${process.env.CD_MAXMS ?? 'def'} wall=${wall.toFixed(0)}s\n`);
        process.stdout.write(`pc  n     NEWwin%  fairwin%   NEWmean_fp  base    dec_mean dec_p99 dec_max (ms)\n`);
        for (const pc of pcs.split(',').map(Number)) {
            const a = agg.get(pc); if (!a) continue;
            const win = 100 * a.wins / Math.max(1, a.n);
            const mean = a.fpSum / Math.max(1, a.n);
            const dMean = a.dec.length ? a.dec.reduce((x, y) => x + y, 0) / a.dec.length : 0;
            process.stdout.write(`${String(pc).padStart(2)}  ${String(a.n).padStart(4)}  ${win.toFixed(1).padStart(6)}  ${(100 / pc).toFixed(1).padStart(7)}    ${mean.toFixed(3)}    ${(1 + (pc - 1) / 2).toFixed(2)}   ${dMean.toFixed(0).padStart(6)}  ${pct(a.dec, 0.99).toFixed(0).padStart(6)}  ${Math.max(0, ...a.dec).toFixed(0).padStart(6)}\n`);
        }
    }
}
