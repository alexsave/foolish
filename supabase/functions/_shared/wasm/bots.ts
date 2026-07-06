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
import { LegalMove } from '../bot_interfaces.ts';
import { takeBOTS_WASM_B64 } from './bots_wasm.ts';
import {
    EngineExports, __LOG_TYPE_TO_INT, __MOVE_TYPE, __adoptEngine,
    __decodeBase64, __marshalGame, __mem, __pooledCard, __setResident,
    __wireLogCard, __cardFromWire,
} from './engine.ts';

interface BotsExports extends EngineExports {
    wasm_import_logs(): void;
    wasm_clear_logs(): void;
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
    semtex: 18,
    octogen: 20,
} as const;

let exportsCache: BotsExports | null = null;

// Memory diagnostics for the edge 150MB-external budget (see utils.ts [MEM]
// logging): current size of the bots.wasm linear memory, -1 until loaded.
export function __botsWasmMB(): number {
    const mem = (exportsCache as unknown as { memory?: WebAssembly.Memory } | null)?.memory;
    return mem ? Math.round(mem.buffer.byteLength / 1048576) : -1;
}

// Eager adoption for bot-serving workers: a fresh worker's first kernel
// touch is otherwise a rules-path helper (kernelLegalMoves), which decodes
// and instantiates rules.wasm only for bots.wasm to adopt over it one call
// later — a whole engine instance built to be abandoned. Calling this at
// the top of the bot loop makes bots.wasm the FIRST and only instance.
export function __ensureBots(): void { bots(); }

function bots(): BotsExports {
    if (exportsCache) return exportsCache;
    const module = new WebAssembly.Module(__decodeBase64(takeBOTS_WASM_B64()) as BufferSource);
    const instance = new WebAssembly.Instance(module, {});
    const ex = instance.exports as unknown as BotsExports;
    ex.wasm_init();
    exportsCache = ex;
    // bots.wasm is a superset of rules.wasm — adopt the engine slot so a bot
    // turn's choose and its follow-up action run on one instance, enabling
    // the resident-state marshal skip (see engine.ts).
    __adoptEngine(ex);
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
// defender_index, u8 num_pairs, num_pairs x (u8 primary, u8 target) 1-byte
// wire cards). Hidden cards travel as 0xFE ({-1,-1} in the log store) — the
// belief-based bots (cordite, fulminate) read these.
const MAX_KERNEL_LOGS = 512;   // MAX_LOGS in cnitro/src/game.h
const MAX_KERNEL_PAIRS = 64;   // MAX_LOG_PAIRS in the wasm build

function importLogs(ex: BotsExports, game: Game): void {
    const logs = game.logs ?? [];
    const buf = __mem(ex);
    let q = ex.wasm_io_ptr();
    const n = Math.min(logs.length, MAX_KERNEL_LOGS);
    buf[q++] = n & 0xff;
    buf[q++] = (n >> 8) & 0xff;
    // Seat lookup once, not a findIndex string scan per log — this function
    // walks the whole (ever-growing) session log per decision and was the
    // single hottest TS frame in the bot-pipeline profile.
    const seatOf = new Map<string, number>();
    game.players.forEach((p, i) => seatOf.set(p.player_id, i));
    for (let i = 0; i < n; i++) {
        const l = logs[i];
        buf[q++] = __LOG_TYPE_TO_INT.get(l.log_type) ?? 0;
        const seat = l.player_id !== null && l.player_id !== undefined
            ? (seatOf.get(l.player_id) ?? -1)
            : -1;
        buf[q++] = seat & 0xff;
        buf[q++] = (l.defender_index ?? -1) & 0xff;
        const pairs = l.card_pairs ?? [];
        const np = Math.min(pairs.length, MAX_KERNEL_PAIRS);
        buf[q++] = np;
        for (let j = 0; j < np; j++) {
            const p = pairs[j];
            // missing primary encodes as the hidden card, matching the old
            // (-1,-1) bytes; missing target is the in-band "no card".
            buf[q++] = p.primary ? __wireLogCard(p.primary) : 0xfe;
            buf[q++] = __wireLogCard(p.target);
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

// Run one full bot decision in the kernel: marshal state (+ public logs for
// the strategies that read them), install knobs, seed the strategy RNG,
// enumerate + choose in C. Returns the chosen index into the same legal-move
// ordering kernelLegalMoves produces, or -1 (unknown player / no legal
// moves). `opts.logs` defaults ON; the registry turns it off for strategies
// that never read the session log — the log marshal was the hottest TS
// frame in the bot-pipeline profile.
export function wasmChooseMove(
    game: Game,
    playerId: string,
    strat: number,
    opts: { env?: Record<string, string>; logs?: boolean } = {},
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
    if (opts.logs !== false) importLogs(ex, game);
    if (opts.env) setEnv(ex, opts.env);
    const seed = seedSource ? seedSource() : Math.floor(Math.random() * 4294967296);
    ex.wasm_set_strategy_seed(seed >>> 0);
    const idx = ex.wasm_choose_move(strat, seat);
    // The choose only READ the marshaled state; clearing the imported logs
    // makes the resident kernel state byte-equivalent to a fresh marshal, so
    // the action that follows on this same game object can skip its own.
    ex.wasm_clear_logs();
    __setResident(game);
    return idx;
}

// Same decision, but the chosen move is read straight from the bytes
// wasm_choose_move wrote to the IO buffer (u8 type, u8 n, cards,
// attack_cards) instead of indexing a TS-materialized move list. This lets
// the bot loop skip kernelLegalMoves' full enumerate-export-parse round
// trip — the kernel enumerates internally either way. Returns null when the
// player is unknown or has no legal moves.
export function wasmChooseMoveDirect(
    game: Game,
    playerId: string,
    strat: number,
    opts: { env?: Record<string, string>; logs?: boolean } = {},
): LegalMove | null {
    const ex = bots();
    const idx = wasmChooseMove(game, playerId, strat, opts);
    if (idx < 0) return null;
    const buf = __mem(ex);
    let q = ex.wasm_io_ptr();
    const type = __MOVE_TYPE[buf[q++]] as LegalMove['type'];
    const k = buf[q++];
    if (type === 'pickup' || type === 'good' || type === 'wait') return { type };
    const cards = new Array(k);
    for (let j = 0; j < k; j++) cards[j] = __cardFromWire(buf[q++]);
    if (type === 'cover') {
        const attacks = new Array(k);
        for (let j = 0; j < k; j++) attacks[j] = __cardFromWire(buf[q++]);
        return { type, cards, attack_cards: attacks };
    }
    return { type, cards };
}
