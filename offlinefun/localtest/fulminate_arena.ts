// Fulminate evaluation arena. Two modes, both fork N tsx workers that each
// play a seeded slice of games and report win% / mean-finish / latency.
//
// MODE "ab" (default, the headline test): a PAIRED same-table A/B. Seat 0 is
// `fulminate`, seat 1 is `cordite`, the remaining seats are filled by cycling
// the comma-list of `fillers`. Both bots face the SAME opponents in the SAME
// games, so the fulminate-minus-cordite delta has low variance (paired). This
// directly answers "does opponent modeling beat cordite, and where?".
//
//   npx tsx offlinefun/localtest/fulminate_arena.ts ab <fillers> <pcs> <games> [workers]
//   npx tsx offlinefun/localtest/fulminate_arena.ts ab random 2,4,6 400 4
//   npx tsx offlinefun/localtest/fulminate_arena.ts ab random,handwritten,espresso 4,6 300 4
//
// MODE "hero": seat 0 is <hero>, the other (n-1) seats are <opp> (the classic
// cordite_arena layout). Run it once with hero=fulminate and once with
// hero=cordite to compare against a baseline.
//
//   npx tsx offlinefun/localtest/fulminate_arena.ts hero <hero> <opp> <pcs> <games> [workers]
//
// Env knobs (CD_WORLDMUL, CD_NO_SOLVE, CD_NO_FASTROLL, CD_MAXMS) propagate to
// workers and affect cordite_core (hence both fulminate and cordite).

import { fork } from 'child_process';
import * as os from 'os';

const isWorker = process.argv.includes('--worker');
if (isWorker) runWorker(); else runMaster();

// Per player-count tallies. In "ab" mode seat0=fulminate, seat1=cordite; we
// track both. In "hero" mode only seat0 is tracked (s1* stay 0).
interface Result {
    pc: number; n: number;
    s0w: number; s0fp: number; s0dec: number[];   // seat 0 (fulminate / hero)
    s1w: number; s1fp: number; s1dec: number[];   // seat 1 (cordite, ab mode)
}

function runWorker(): void {
    Promise.resolve().then(async () => {
        const { calculateLegalMoves, getBotStrategy } =
            await import('../../supabase/functions/_shared/bot_strategy.ts');
        const { shouldBotActCore, executeBotMove } = await import('../../supabase/functions/_shared/pure_bot_actions.ts');
        const { start_game, game_done } = await import('../../supabase/functions/_shared/common_utils.ts');
        const { GAME_STATUS, PLAYER_STATUS } = await import('../../supabase/functions/_shared/types.ts');

        const mode = process.env.AR_MODE!;
        const pcs = process.env.AR_PCS!.split(',').map(Number);
        const games = Number(process.env.AR_GAMES);
        const seed0 = Number(process.env.AR_SEED);

        if (process.env.CD_WORLDMUL) (globalThis as any).CD_WORLDMUL = Number(process.env.CD_WORLDMUL);
        if (process.env.FUL_OFF) (globalThis as any).FUL_OFF = true;
        if (process.env.CD_NO_SOLVE) (globalThis as any).CD_NO_SOLVE = true;
        if (process.env.CD_NO_FASTROLL) (globalThis as any).CD_NO_FASTROLL = true;
        if (process.env.CD_MAXMS) {
            const core = await import('../../supabase/functions/_shared/strategies/cordite_core.ts');
            (core.CORDITE_PARAMS as any).maxMillis = Number(process.env.CD_MAXMS);
            (core.CORDITE_MAX_PARAMS as any).maxMillis = Number(process.env.CD_MAXMS);
        }

        let _seed = seed0 >>> 0 || 1;
        Math.random = () => { _seed = (_seed * 1664525 + 1013904223) % 4294967296; return _seed / 4294967296; };
        console.log = () => {}; console.warn = () => {}; console.error = () => {};

        // Seat strategy layout for a given player count.
        const fillers = (process.env.AR_FILLERS ?? 'random').split(',');
        const hero = process.env.AR_HERO ?? 'fulminate';
        const opp = process.env.AR_OPP ?? 'random';
        const seatKeys = (n: number): string[] => {
            const keys: string[] = [];
            if (mode === 'ab') {
                keys.push('fulminate');                         // seat 0
                keys.push('cordite');                           // seat 1
                for (let i = 2; i < n; i++) keys.push(fillers[(i - 2) % fillers.length]);
            } else {
                keys.push(hero);
                for (let i = 1; i < n; i++) keys.push(opp);
            }
            return keys;
        };

        const mkP = (s: string, i: number): any => ({ player_id: `b${i}`, name: `${s}${i}`,
            status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: s });
        const mkG = (keys: string[]): any => ({ players: keys.map((s, i) => mkP(s, i)),
            deck: [], logs: [], id: 'l', name: 'l', status: GAME_STATUS.PLAYING, deck_length: 0, discard_pile_length: 0,
            flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
            good_timestamp: null, good_players: [] });

        const results: Result[] = [];
        for (const pc of pcs) {
            const keys = seatKeys(pc);
            const r: Result = { pc, n: 0, s0w: 0, s0fp: 0, s0dec: [], s1w: 0, s1fp: 0, s1dec: [] };
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
                        const t0 = performance.now();
                        const mv = await st.chooseMove(game, pl.player_id, lm);
                        const dt = performance.now() - t0;
                        if (lm.length > 1) {
                            if (pi === 0) r.s0dec.push(dt);
                            else if (pi === 1 && mode === 'ab') r.s1dec.push(dt);
                        }
                        if (executeBotMove(game, pl, mv)) { acted = true; break; }
                    }
                    if (!acted) break;
                }
                if (game_done(game) === null) continue;
                const fpOf = (seat: number): number => {
                    const pos = game.elimination_order.indexOf(game.players[seat].player_id);
                    return pos >= 0 ? pos + 1 : pc;
                };
                const f0 = fpOf(0);
                r.n++; r.s0fp += f0; if (f0 === 1) r.s0w++;
                if (mode === 'ab') { const f1 = fpOf(1); r.s1fp += f1; if (f1 === 1) r.s1w++; }
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
function mean(arr: number[]): number { return arr.length ? arr.reduce((x, y) => x + y, 0) / arr.length : 0; }

function runMaster(): void {
    const mode = process.argv[2] ?? 'ab';
    let pcs: string, games: number, workers: number, label: string;
    const env: Record<string, string> = { ...process.env as any, AR_MODE: mode };
    if (mode === 'ab') {
        const fillers = process.argv[3] ?? 'random';
        pcs = process.argv[4] ?? '2,4,6';
        games = Number(process.argv[5] ?? 300);
        workers = Number(process.argv[6] ?? Math.min(4, os.cpus().length));
        env.AR_FILLERS = fillers;
        label = `AB  seat0=fulminate seat1=cordite fillers=[${fillers}]`;
    } else {
        const hero = process.argv[3] ?? 'fulminate';
        const opp = process.argv[4] ?? 'random';
        pcs = process.argv[5] ?? '2,4,6';
        games = Number(process.argv[6] ?? 300);
        workers = Number(process.argv[7] ?? Math.min(4, os.cpus().length));
        env.AR_HERO = hero; env.AR_OPP = opp;
        label = `HERO seat0=${hero} opp=${opp}`;
    }
    const per = Math.ceil(games / workers);
    env.AR_PCS = pcs; env.AR_GAMES = String(per);

    const agg = new Map<number, Result>();
    let done = 0; const t0 = Date.now();
    for (let w = 0; w < workers; w++) {
        const child = fork(__filename, ['--worker'], {
            env: { ...env, AR_SEED: String(100003 + w * 7919) },
            execArgv: ['--import', 'tsx'],
        });
        child.on('message', (msg: any) => {
            for (const r of msg.results as Result[]) {
                let a = agg.get(r.pc);
                if (!a) { a = { pc: r.pc, n: 0, s0w: 0, s0fp: 0, s0dec: [], s1w: 0, s1fp: 0, s1dec: [] }; agg.set(r.pc, a); }
                a.n += r.n; a.s0w += r.s0w; a.s0fp += r.s0fp; a.s1w += r.s1w; a.s1fp += r.s1fp;
                for (const d of r.s0dec) a.s0dec.push(d);
                for (const d of r.s1dec) a.s1dec.push(d);
            }
        });
        child.on('exit', () => { if (++done === workers) report(); });
    }
    function report(): void {
        const wall = (Date.now() - t0) / 1000;
        process.stdout.write(`${label}  games/pc=${[...agg.values()][0]?.n ?? 0} workers=${workers} MAXMS=${process.env.CD_MAXMS ?? 'def'} WMUL=${process.env.CD_WORLDMUL ?? 1} wall=${wall.toFixed(0)}s\n`);
        if (mode === 'ab') {
            process.stdout.write(`pc  n     ful_win% cor_win%  dWin  ful_mfp cor_mfp  dMFP   f_decMean f_p99 f_max (ms)\n`);
            for (const pc of pcs.split(',').map(Number)) {
                const a = agg.get(pc); if (!a) continue;
                const fw = 100 * a.s0w / Math.max(1, a.n), cw = 100 * a.s1w / Math.max(1, a.n);
                const fm = a.s0fp / Math.max(1, a.n), cm = a.s1fp / Math.max(1, a.n);
                process.stdout.write(`${String(pc).padStart(2)}  ${String(a.n).padStart(4)}  ${fw.toFixed(1).padStart(6)}  ${cw.toFixed(1).padStart(6)}  ${(fw - cw >= 0 ? '+' : '') + (fw - cw).toFixed(1)}  ${fm.toFixed(3)}  ${cm.toFixed(3)}  ${(fm - cm >= 0 ? '+' : '') + (fm - cm).toFixed(3)}  ${mean(a.s0dec).toFixed(0).padStart(7)}  ${pct(a.s0dec, 0.99).toFixed(0).padStart(4)}  ${Math.max(0, ...a.s0dec).toFixed(0).padStart(4)}\n`);
            }
            process.stdout.write(`(dWin/dMFP = fulminate minus cordite; +dWin good, -dMFP good)\n`);
        } else {
            process.stdout.write(`pc  n     win%   mean_fp  base   dec_mean dec_p99 dec_max (ms)\n`);
            for (const pc of pcs.split(',').map(Number)) {
                const a = agg.get(pc); if (!a) continue;
                const win = 100 * a.s0w / Math.max(1, a.n), mfp = a.s0fp / Math.max(1, a.n);
                process.stdout.write(`${String(pc).padStart(2)}  ${String(a.n).padStart(4)}  ${win.toFixed(1).padStart(5)}  ${mfp.toFixed(3)}   ${(1 + (pc - 1) / 2).toFixed(2)}  ${mean(a.s0dec).toFixed(0).padStart(7)}  ${pct(a.s0dec, 0.99).toFixed(0).padStart(6)}  ${Math.max(0, ...a.s0dec).toFixed(0).padStart(6)}\n`);
            }
        }
    }
}
