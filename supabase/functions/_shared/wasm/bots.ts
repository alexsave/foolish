// TypeScript bridge to the cnitro BOT module compiled to WebAssembly.
//
// bots.wasm is the rules kernel (same sources as rules.wasm) PLUS every
// algorithmic bot strategy (cnitro/src/*_strategy.c) and a choose-move
// bridge (cnitro/wasm/wasm_bots_api.c). The C sources are the single source
// of truth for bot play; this module only marshals the game in and reads the
// chosen move index out. The whole bot turn — legal-move enumeration,
// belief building, Monte-Carlo deliberation — runs inside the module.
//
// The chosen INDEX maps 1:1 onto the LegalMove list the server computed via
// kernelLegalMoves, because both come from the same C enumerator
// (cnitro/src/legal.c calculate_legal_moves) over the same marshaled state.
//
// Loaded lazily and cached: the pure rules path (actions/) never pays for
// the larger module; the first bot decision instantiates it.

import { Game } from '../types.ts';
import { BOTS_WASM_B64 } from './bots_wasm.ts';
import {
    EngineExports, __LOG_TYPE_TO_INT, __decodeBase64, __marshalGame, __mem,
} from './engine.ts';

interface BotsExports extends EngineExports {
    wasm_import_logs(): void;
    wasm_import_strategy_keys(): void;
    wasm_set_game_key(key: number): void;
    wasm_setenv_from_io(): void;
    wasm_clearenv(): void;
    wasm_reload_bot_flags(): void;
    wasm_set_strategy_seed(s: number): void;
    wasm_choose_move(strat: number, botIdx: number): number;
}

// Mirrors STRAT_* in cnitro/src/strategy.h (only the ids the server uses).
// espresso/handwritten map to the *_PROD mirrors of the production TS bots;
// the kernel's un-suffixed variants (ids 1/2) are the arena/cordite-rollout
// versions, which drifted slightly and stay frozen for cordite's sake.
export const STRAT = {
    random: 0,
    espresso: 15,
    handwritten: 16,
    cordite: 7,
    simple_heuristic: 10,
    champion: 11,
    ultimate_champion: 12,
    hacker: 13,
    fulminate: 14,
} as const;

let exportsCache: BotsExports | null = null;

function bots(): BotsExports {
    if (exportsCache) return exportsCache;
    const module = new WebAssembly.Module(__decodeBase64(BOTS_WASM_B64) as BufferSource);
    const instance = new WebAssembly.Instance(module, {});
    const ex = instance.exports as unknown as BotsExports;
    ex.wasm_init();
    exportsCache = ex;
    return ex;
}

// Strategy RNG. Each decision seeds the kernel's dedicated strategy LCG
// (cnitro/src/game.c random_strategy_random) with one fresh draw, so
// stochastic strategies stay stochastic across decisions. The parity/e2e
// harness can pin the stream (mirrors engine.ts __setKernelSeedSource).
let seedSource: (() => number) | null = null;
export function __setBotSeedSource(fn: (() => number) | null): void { seedSource = fn; }

// Session log marshal-in — wire layout of cnitro/wasm/wasm_bots_api.c
// wasm_import_logs (u16 count; per log: i8 type, i8 player seat, i8
// defender_index, u8 num_pairs, num_pairs x (i8 ps, i8 pv, i8 ts, i8 tv,
// u8 has_target)). Hidden cards travel as {-1,-1}, exactly how the log
// store keeps them — the belief-based bots (cordite, fulminate) read these.
const MAX_KERNEL_LOGS = 512;   // MAX_LOGS in cnitro/src/game.h
const MAX_KERNEL_PAIRS = 64;   // MAX_LOG_PAIRS in the wasm build

function importLogs(ex: BotsExports, game: Game): void {
    const logs = game.logs ?? [];
    const buf = __mem(ex);
    let q = ex.wasm_io_ptr();
    const n = Math.min(logs.length, MAX_KERNEL_LOGS);
    buf[q++] = n & 0xff;
    buf[q++] = (n >> 8) & 0xff;
    for (let i = 0; i < n; i++) {
        const l = logs[i];
        buf[q++] = __LOG_TYPE_TO_INT[l.log_type] ?? 0;
        const seat = l.player_id !== null && l.player_id !== undefined
            ? game.players.findIndex(p => p.player_id === l.player_id)
            : -1;
        buf[q++] = seat & 0xff;
        buf[q++] = (l.defender_index ?? -1) & 0xff;
        const pairs = l.card_pairs ?? [];
        const np = Math.min(pairs.length, MAX_KERNEL_PAIRS);
        buf[q++] = np;
        for (let j = 0; j < np; j++) {
            const p = pairs[j];
            buf[q++] = (p.primary?.suit ?? -1) & 0xff;
            buf[q++] = (p.primary?.value ?? -1) & 0xff;
            buf[q++] = (p.target?.suit ?? -1) & 0xff;
            buf[q++] = (p.target?.value ?? -1) & 0xff;
            buf[q++] = p.target !== null && p.target !== undefined ? 1 : 0;
        }
    }
    ex.wasm_import_logs();
}

// Seat strategy keys: espresso's opponent modeling branches on whether an
// opponent is the 'random' bot, so each seat's key rides along as a STRAT_*
// id (-1 for keys with no kernel id: human, gpt, console, nitro).
function importStrategyKeys(ex: BotsExports, game: Game): void {
    const buf = __mem(ex);
    const q = ex.wasm_io_ptr();
    for (let i = 0; i < game.players.length; i++) {
        const key = (game.players[i] as { strategy_key?: string }).strategy_key;
        const id = key !== undefined && key in STRAT ? STRAT[key as keyof typeof STRAT] : -1;
        buf[q + i] = id & 0xff;
    }
    ex.wasm_import_strategy_keys();
}

// Tuning knobs (CD_* for the Monte-Carlo bots) go through the kernel's tiny
// env table. Rewritten per decision because one module instance serves
// strategies with different knobs (cordite vs cordite_max).
function setEnv(ex: BotsExports, env: Record<string, string>): void {
    ex.wasm_clearenv();
    const base = ex.wasm_io_ptr();
    for (const [key, value] of Object.entries(env)) {
        const buf = __mem(ex);
        let q = base;
        for (let i = 0; i < key.length; i++) buf[q++] = key.charCodeAt(i) & 0xff;
        buf[q++] = 0;
        for (let i = 0; i < value.length; i++) buf[q++] = value.charCodeAt(i) & 0xff;
        buf[q++] = 0;
        ex.wasm_setenv_from_io();
    }
    ex.wasm_reload_bot_flags();
}

// Run one full bot decision in the kernel: marshal state + public logs,
// install knobs, seed the strategy RNG, enumerate + choose in C. Returns the
// chosen index into the same legal-move ordering kernelLegalMoves produces,
// or -1 (unknown player / no legal moves).
export function wasmChooseMove(
    game: Game,
    playerId: string,
    strat: number,
    opts: { env?: Record<string, string> } = {},
): number {
    const seat = game.players.findIndex(p => p.player_id === playerId);
    if (seat < 0) return -1;
    const ex = bots();
    __marshalGame(ex, game);
    // Per-game bot memory (espresso's discard set was keyed by game id in
    // TS): FNV-1a of game.id, so a new game resets, the same game resumes.
    let h = 0x811c9dc5;
    for (let i = 0; i < game.id.length; i++) {
        h ^= game.id.charCodeAt(i);
        h = Math.imul(h, 0x01000193);
    }
    ex.wasm_set_game_key(h >>> 0);
    importStrategyKeys(ex, game);
    importLogs(ex, game);
    if (opts.env) setEnv(ex, opts.env);
    const seed = seedSource ? seedSource() : Math.floor(Math.random() * 4294967296);
    ex.wasm_set_strategy_seed(seed >>> 0);
    return ex.wasm_choose_move(strat, seat);
}
