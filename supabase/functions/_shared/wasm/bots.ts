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
import { loadWasmGz } from './wasm_asset.ts';
import {
    EngineExports, PackedRunOk, __LOG_TYPE_TO_INT, __MOVE_TYPE, __adoptEngine,
    __marshalGame, __mem, __pooledCard, __replayError, __setResident,
    __wireLogCard, __cardFromWire, applyKernelStateToGame,
    exportPackedDriveProducts, rngBaseFromSeed,
} from './engine.ts';

interface BotsExports extends EngineExports {
    wasm_import_logs(): void;
    wasm_replay_encode_v6_from_game(max_atoms: number): number;
    wasm_replay_events(viewer: number, from: number, code_len: number): number;
    wasm_replay_events_n(): number;
    wasm_replay_events_next(): number;
    wasm_replay_step_count(code_len: number): number;
    wasm_clear_logs(): void;
    wasm_import_strategy_keys(): void;
    wasm_set_game_key(key: number): void;
    wasm_setenv_from_io(): void;
    wasm_clearenv(): void;
    wasm_reload_bot_flags(): void;
    wasm_set_strategy_seed(s: number): void;
    wasm_choose_move(strat: number, botIdx: number): number;
    // The drive cycle (docs/C_CORE_CONSOLIDATION.md F2/F3)
    wasm_bot_eligible_mask(humanMask: number): number;
    wasm_bot_pacing_ms(pacingClass: number, humansPresent: number): number;
    wasm_bot_drive(humanMask: number, maxActions: number, nPref: number): number;
    wasm_bot_drive_log_start(): number;
    // Belief probe (observability; off until reset arms it)
    wasm_belief_probe_reset(): void;
    wasm_belief_probe_dump(): number;
}

// Mirrors STRAT_* in cnitro/src/strategy.h (only the ids the server uses).
// espresso/handwritten map to the *_PROD mirrors of the production TS bots;
// the kernel's un-suffixed variants (ids 1/2) are the arena/cordite-rollout
// versions, which drifted slightly and stay frozen for cordite's sake.
export const STRAT = {
    random: 0,
    espresso: 15,
    handwritten: 16,
    firecracker: 4,
    blackpowder: 6,
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

// Raw linear-memory bytes (wasm grows in 64KB pages) — finer than the MB round
// above for benchmarks that need to see small deltas.
export function __botsWasmBytes(): number {
    const mem = (exportsCache as unknown as { memory?: WebAssembly.Memory } | null)?.memory;
    return mem ? mem.buffer.byteLength : -1;
}

// Eager adoption for bot-serving workers: a fresh worker's first kernel
// touch is otherwise a rules-path helper (kernelLegalMoves), which decodes
// and instantiates rules.wasm only for bots.wasm to adopt over it one call
// later — a whole engine instance built to be abandoned. Calling this at
// the top of the bot loop makes bots.wasm the FIRST and only instance.
export function __ensureBots(): void { bots(); }

// Analysis only: read the per-decision deliberation dump from a bots.wasm built
// with -DOG_EXPLAIN_BUILD (make bots-wasm-explain). Returns '' for the shipped
// build (no such export). Reset clears the static buffer for the next decision.
export function __ogExplainDump(reset = true): string {
    const ex = bots() as unknown as {
        wasm_og_explain_ptr?: () => number; wasm_og_explain_len?: () => number;
        wasm_og_explain_reset?: () => void; memory: WebAssembly.Memory;
    };
    if (!ex.wasm_og_explain_len || !ex.wasm_og_explain_ptr) return '';
    const len = ex.wasm_og_explain_len();
    const ptr = ex.wasm_og_explain_ptr();
    const s = new TextDecoder().decode(new Uint8Array(ex.memory.buffer, ptr, len));
    if (reset && ex.wasm_og_explain_reset) ex.wasm_og_explain_reset();
    return s;
}

// Test-only: pin the mid-game DRAW LCG to a pure function of the currently
// marshaled state — the same value packedActionCore/the drive's apply phase
// would set. A decision does not re-seed this stream (only an apply does), so a
// Monte-Carlo bot that reads it (robusta_strategy.c, and firecracker through it
// — unlike blackpowder/cordite/octogen, which save and restore it) samples from
// whatever the LAST apply left. Within one game that is a pure function of its
// own history; across games sharing a module instance it carries over, so a
// harness that plays several games through one instance calls this to start
// each from the same stream.
export function __seedDrawRngFromState(): void { bots().wasm_seed_rng_deterministic(); }

// Analysis only: the strategy RNG seed (state_fnv) for the CURRENTLY marshaled
// state. Call right after a choose to read the seed that decision used — lets a
// harness confirm the seed varies per decision yet reproduces across a replay.
export function __strategySeedProbe(): number {
    const ex = bots() as unknown as { wasm_strategy_seed_probe?: () => number };
    return ex.wasm_strategy_seed_probe ? (ex.wasm_strategy_seed_probe() >>> 0) : -1;
}

function bots(): BotsExports {
    if (exportsCache) return exportsCache;
    const module = new WebAssembly.Module(loadWasmGz('bots') as BufferSource);
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

// C-buffer fast path: splice the session log's PACKED bytes (games.logs_packed,
// logwire format — see wire/logwire.ts) straight into the kernel import buffer,
// with NO JS GameLog[] in between. The two layouts differ only in that logwire
// prepends a u48 timestamp per record and has no count header; the records
// themselves (type, seat, defender, n_pairs, wire-card pairs) are byte-identical
// to what wasm_import_logs wants — and the seat is already in the bytes, so the
// old decode→player_id→re-marshal→seat round trip was pure waste. This is the
// "keep logs as C buffers" path: DB bytes → kernel, one copy.
function importLogsPacked(ex: BotsExports, bytes: Uint8Array): void {
    const buf = __mem(ex);
    const base = ex.wasm_io_ptr();
    let w = base + 2;   // records go after the u16 count header
    let p = 0, n = 0;   // read cursor into logwire bytes; records written
    while (p + 10 <= bytes.length && n < MAX_KERNEL_LOGS) {
        p += 6;                                  // skip the u48 timestamp
        const type = bytes[p], seat = bytes[p + 1], def = bytes[p + 2];
        const srcPairs = bytes[p + 3];
        p += 4;
        if (p + srcPairs * 2 > bytes.length) break;   // truncated tail — stop
        const wPairs = srcPairs < MAX_KERNEL_PAIRS ? srcPairs : MAX_KERNEL_PAIRS;
        buf[w++] = type; buf[w++] = seat; buf[w++] = def; buf[w++] = wPairs;
        for (let j = 0; j < wPairs * 2; j++) buf[w++] = bytes[p + j];
        p += srcPairs * 2;
        n++;
    }
    buf[base] = n & 0xff;
    buf[base + 1] = (n >> 8) & 0xff;
    ex.wasm_import_logs();
}

function importLogs(ex: BotsExports, game: Game): void {
    // Fast path: the server bot loop hands the belief bots the session log as
    // its raw packed bytes (game.belief_log_bytes) — feed them to the kernel
    // with zero JS-object marshaling. Offline/test harnesses have no bytes and
    // accumulate JS logs in game.logs (or set game.belief_logs directly), so the
    // object marshal below still covers them.
    const packed = game.belief_log_bytes;
    if (packed) { importLogsPacked(ex, packed); return; }
    const logs = game.belief_logs ?? game.logs ?? [];
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
// id (-1 for keys with no kernel id: human, gpt, console).
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

// Tuning knobs (CD_*/OG_* for the Monte-Carlo bots) go through the kernel's
// tiny env table. Rewritten per decision because one module instance serves
// strategies with different knobs (cordite's CD_* vs octogen's OG_*), and the
// bots latch their knobs on first read.
//
// The knob VALUES are canonically the C roster's (cnitro/src/bot_roster.c);
// env overrides it, so what this writes is what the server plays
// (docs/C_CORE_CONSOLIDATION.md F1). e2e/bot_roster_parity.test.ts holds the
// two in lockstep until the env table is deleted.
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
    // A choose is a state READER and must reflect the game object EXACTLY as it
    // is right now. Never let it consume a resident mark left by a prior
    // decision: the bot loop reuses one game object across decisions and mutates
    // it out-of-band (state reload on a CAS conflict, round-transition refill,
    // passive-action bundling), so a stale resident kernel state can differ from
    // the live object — e.g. an already-dead deck still reading as alive, which
    // silently gates off the exact endgame solver. Force a fresh marshal.
    __setResident(null);
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
    // Tests pin the strategy stream via seedSource. Live, seed it
    // DETERMINISTICALLY — but from the SERVER-ONLY deal seed (game.game_seed),
    // not the public board. The Monte-Carlo bots' rollout opponent models draw
    // from this stream; if the seed were a hash of visible state, a source-code
    // holder could recompute it and predict octogen's every move. wasm_set_rng_base
    // folds the (never-client-visible) deal seed in, so the seed is reproducible
    // ONLY to the server that holds it. Replaces the old per-decision Math.random.
    if (seedSource) ex.wasm_set_strategy_seed(seedSource() >>> 0);
    else { ex.wasm_set_rng_base(rngBaseFromSeed(game.game_seed)); ex.wasm_set_strategy_seed_deterministic(); }
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

// ---------------------------------------------------------------------------
// The bot drive cycle (docs/C_CORE_CONSOLIDATION.md F2/F3)
//
// One call per cycle: the kernel finds the eligible bot seats, shuffles them
// fairly, chooses through the roster, applies, bundles the silent actions and
// stops on the first visible one. What used to be the server loop's inner
// cycle (bot_actions.ts) and the phone's seat walk is now this, once, in C.
//
// What stays on this side is marshaling and the I/O the kernel cannot do: the
// belief-log hydration (a DB read) and the deal seed (a secret). Everything
// that decides what HAPPENS — eligibility, fairness, choice, bundling, pacing
// class, per-decision seeding — is kernel property.
// ---------------------------------------------------------------------------

// BOT_STOP_* / BOT_PACE_* — cnitro/src/bot_drive.h.
export const BOT_STOP = { NO_ELIGIBLE: 0, ENDED: 1, EVENTS: 2, MAX: 3 } as const;
export const BOT_PACE = { NONE: 0, BUNDLED_PASSIVE: 1, MOVE: 2, ROUND_TRANSITION: 3 } as const;

export interface BotDriveAction {
    seat: number;
    pacingClass: number;   // BOT_PACE_*
    move: LegalMove;
}

// A move a seat already chose in a CAS attempt that then conflicted, offered
// back so it need not search again (cordite's Monte-Carlo is seconds of CPU
// and the edge budget is ~2s). The kernel replays it only if the reloaded
// state still makes it legal — legality is never taken on our word.
export interface BotDrivePref {
    seat: number;
    move: LegalMove;
}

export interface BotDriveResult {
    actions: BotDriveAction[];
    stop: number;            // BOT_STOP_*
    ended: number;           // the fool's seat, or -1
    // The whole cycle's products, or null when it applied nothing: the final
    // state blob, every bundled action's log records concatenated, and the
    // per-viewer event streams. Exactly what the commit takes.
    run: PackedRunOk | null;
}

// Which bot seats could act right now. The server calls this BEFORE driving to
// decide an I/O the kernel cannot do: hydrating the DRAW-masked session log,
// but only when a bot that reads it is about to choose.
//
// Deliberately leaves no resident mark: the caller awaits a DB read before it
// drives, and a reader that trusted a mark across that would search a state
// the game object has since moved past.
export function wasmBotEligibleMask(game: Game, humanMask: number): number {
    const ex = bots();
    __setResident(null);
    __marshalGame(ex, game);
    return ex.wasm_bot_eligible_mask(humanMask) >>> 0;
}

/** One recorded bot SEARCH, as the kernel saw it (wasm_belief_probe_dump). */
export interface BeliefProbeRecord {
    seat: number;
    /** Log records spliced into the Game the strategy was about to read. */
    nLogs: number;
    /** Real cards visible in that log — `${suit}:${value}` ids. */
    cards: Set<string>;
}

/**
 * Arm the kernel's belief probe (clears any prior records).
 *
 * Observability for "did the bot actually SEE the session log", answered by the
 * kernel instead of inferred from this side. A spy here could only prove the
 * bytes were handed over, not that the importer spliced them into the Game the
 * strategy read — and since the choose step moved in-kernel (F2/A2) there is no
 * TS seam left to spy on. Off in production until this is called.
 */
export function wasmBeliefProbeReset(): void { bots().wasm_belief_probe_reset(); }

/** Read back the searches recorded since the last reset, in order. */
export function wasmBeliefProbeDump(): BeliefProbeRecord[] {
    const ex = bots();
    const n = ex.wasm_belief_probe_dump();
    const base = ex.wasm_io_ptr();
    const buf = new Uint8Array(ex.memory.buffer, base, n * 11);
    const out: BeliefProbeRecord[] = [];
    for (let i = 0; i < n; i++) {
        const o = i * 11;
        const cards = new Set<string>();
        for (let b = 0; b < 8; b++) {
            const byte = buf[o + 3 + b];
            for (let bit = 0; bit < 8; bit++) {
                if (!(byte & (1 << bit))) continue;
                const code = b * 8 + bit;          // suit*16 + value
                cards.add(`${code >> 4}:${code & 0xf}`);
            }
        }
        out.push({ seat: buf[o], nLogs: buf[o + 1] | (buf[o + 2] << 8), cards });
    }
    return out;
}

// class -> milliseconds, from the kernel's one pacing table. Never mirrored
// here: the whole point of F3 is that the site and the phone cannot answer
// "how long is this worth watching" differently.
export function wasmBotPacingMs(pacingClass: number, humansPresent: boolean): number {
    return bots().wasm_bot_pacing_ms(pacingClass, humansPresent ? 1 : 0);
}

// Per-game bot memory: FNV-1a of game.id, so a new game resets and the same
// game resumes.
function gameKey(game: Game): number {
    let h = 0x811c9dc5;
    for (let i = 0; i < game.id.length; i++) {
        h ^= game.id.charCodeAt(i);
        h = Math.imul(h, 0x01000193);
    }
    return h >>> 0;
}

// Preferred moves into the IO buffer — the input half of wasm_bot_drive's
// layout (u8 seat, u8 type, u8 n_cards, cards, attack_cards).
function writePrefs(ex: BotsExports, prefs: BotDrivePref[]): number {
    const buf = __mem(ex);
    let q = ex.wasm_io_ptr();
    for (const p of prefs) {
        const cards = p.move.cards ?? [];
        buf[q++] = p.seat & 0xff;
        buf[q++] = __MOVE_TYPE.indexOf(p.move.type) & 0xff;
        buf[q++] = cards.length & 0xff;
        for (const c of cards) buf[q++] = __wireLogCard(c);
        // Only cover reads these, but the layout is fixed-shape either way.
        for (let i = 0; i < cards.length; i++) buf[q++] = __wireLogCard(p.move.attack_cards?.[i]);
    }
    return prefs.length;
}

export function wasmBotDrive(
    game: Game,
    opts: {
        humanMask: number;      // seats the kernel must NOT drive
        aiMask: number;         // for the win finalize (which seats park READY)
        humanSeats: number[];   // event stream recipients
        logs?: boolean;         // hydrate the session log (a belief bot is eligible)
        prefs?: BotDrivePref[];
        maxActions?: number;    // 0/omitted = the kernel's own cap
    },
): BotDriveResult {
    const ex = bots();
    // Force a fresh marshal: the loop reuses one game object across cycles and
    // mutates it out of band (state reload after a CAS conflict, refill), so a
    // resident mark can name a state the kernel no longer holds — an already
    // dead deck reading as alive silently gated off cordite's endgame solver.
    __setResident(null);
    __marshalGame(ex, game);
    ex.wasm_set_game_key(gameKey(game));
    importStrategyKeys(ex, game);
    if (opts.logs) importLogs(ex, game);

    // No env: the C roster's knob specs are authoritative on this path
    // (bot_roster_choose installs them per seat). One module instance serves
    // bots with different knobs, so a table left by an earlier decision would
    // otherwise override the roster for the whole cycle — env beats roster by
    // design (bot_knobs.h), which is right for a research override and wrong
    // for a stale leftover. The values are identical either way
    // (e2e/bot_roster_parity.test.ts pins them knob-for-knob).
    ex.wasm_clearenv();
    ex.wasm_reload_bot_flags();

    // The one seeding call left on this side: the deal seed is a server-only
    // secret the kernel cannot derive. Everything else about what a decision
    // draws is the kernel's (bot_drive_pre_action_hook re-seeds both streams
    // from state_fnv per decision, exactly as the one-move-per-call path did).
    ex.wasm_set_rng_base(rngBaseFromSeed(game.game_seed));

    const nPref = opts.prefs?.length ? writePrefs(ex, opts.prefs) : 0;
    const n = ex.wasm_bot_drive(opts.humanMask, opts.maxActions ?? 0, nPref);
    if (n < 0) throw new Error('bot drive rejected its input');

    // Read the actions out BEFORE anything else touches the IO buffer.
    const buf = __mem(ex);
    let q = ex.wasm_io_ptr();
    const stop = buf[q++];
    const ended = (buf[q++] << 24) >> 24;   // i8
    const count = buf[q++];
    const actions: BotDriveAction[] = [];
    for (let i = 0; i < count; i++) {
        const seat = buf[q++];
        const pacingClass = buf[q++];
        const type = __MOVE_TYPE[buf[q++]] as LegalMove['type'];
        const k = buf[q++];
        const cards = new Array(k);
        for (let j = 0; j < k; j++) cards[j] = __cardFromWire(buf[q++]);
        const attacks = new Array(k);
        for (let j = 0; j < k; j++) attacks[j] = __cardFromWire(buf[q++]);
        const move: LegalMove = type === 'cover' ? { type, cards, attack_cards: attacks }
            : (type === 'pickup' || type === 'good' || type === 'wait') ? { type }
            : { type, cards };
        actions.push({ seat, pacingClass, move });
    }

    if (count === 0) return { actions, stop, ended, run: null };

    // The cycle's products. The actor is the last action's seat: bundled
    // passives are zero-event by definition, so the one event-bearing action
    // is the one that ended the cycle.
    const logStart = ex.wasm_bot_drive_log_start();
    const run = exportPackedDriveProducts(
        actions[actions.length - 1].seat, opts.aiMask, opts.humanSeats, logStart);

    // The kernel has moved; the caller's object has not. Callers hold `game`
    // across the cycle (the loop reuses one object, and the commit reads it for
    // the JSONB dual), so the post-cycle state goes back onto it in place —
    // what executeBotMovePacked does per move.
    //
    // Once per ACTOR, in order, against the final state: every field is
    // overwritten from the same state so the last write wins, but good_players
    // is insertion-ordered and only the actor's own apply can add it. Replaying
    // the actors reproduces the one-call-per-move sequence exactly — including
    // when a cycle's last action clears the set (a pickup or a transition ends
    // the cycle, and then every apply sees the empty mask and empties it).
    for (const a of actions) {
        applyKernelStateToGame(game, run.post, game.players[a.seat].player_id);
    }
    return { actions, stop, ended, run };
}

// ---------- v6 replay production (docs/C_CORE_CONSOLIDATION.md F5/A4) --------

// Encode a finished (or in-progress) seeded game as a v6 replay — the format
// that carries every hidden card's real identity, so a decoder never retrodicts
// a hand.
//
// This is ONE kernel call where the TS choreography used to be ~390 lines
// across three modules and two wasm round-trips: re-deal from the seed
// (reconstructSeededDeal), assemble the reveal stream and the action stream
// (collectV6), hand-marshal them into the codec's byte layout (marshalInputV6),
// then encode. The kernel now re-derives the deal itself and reads the actions
// out of the session log it was handed, so nothing here knows what a reveal
// stream is. The phone makes the same call (fio_replay_encode_v6_b32) — which
// is the point: a third client implements none of this.
//
// It runs on the BOTS module, not the rules module, for one hard reason: this
// needs the whole session log resident, and rules.wasm is built at MAX_LOGS=128
// with no log import (raising it would blow that module's pinned 3-page memory).
// bots.wasm is a superset and adopts the engine slot, so a game that already
// drove bots pays nothing here.
//
// LIMIT: resident means MAX_KERNEL_LOGS. A session longer than that cannot be v6
// and throws, so the caller falls back to v5 — the old TS path had no such
// ceiling (it marshalled actions and never stored a log). Measured: no
// human-plausible game comes close (longest 413 of 512 with a `random` seat in
// it), but all-`random` bot games hit it ~29% of the time. NOT truncated on
// purpose: a short log is a short ACTION stream, and v6 would encode that as a
// perfectly legal mid-game cut — a silently half-recorded game is worse than a
// v5 one. See docs/C_CORE_CONSOLIDATION.md §4.5 for the two ways out.
//
//   game     the played game — the roster (seat order) and the final state.
//   seed     the game's 32-byte deal seed (games.game_seed), hex. This is the
//            server-only column: it re-derives the true deal.
//   logs     the session log as packed bytes (games.logs_packed), or omit to
//            use game.logs / game.belief_log_bytes.
//   maxAtoms cap on atoms — v6's mid-game cut. Default: the whole game.
//
// Throws on a kernel error (including a seed that did not deal this game, which
// the kernel catches by checking the re-dealt trump against the game's own).
export function kernelReplayEncodeV6FromGame(
    game: Game,
    seed: Uint8Array,
    logs?: Uint8Array,
    maxAtoms = 1 << 30,
): Uint8Array {
    if (seed.length !== 32) {
        throw new Error(`replay: v6 needs a 32-byte deal seed, got ${seed.length}`);
    }
    const ex = bots();
    // Always marshal fresh. A cached resident state can be stale in exactly the
    // way that matters here — a dead deck still reading as alive — and this is a
    // reader (see the residentFor note in engine.ts).
    __setResident(null);
    __marshalGame(ex, game);
    if (logs) importLogsPacked(ex, logs);
    else importLogs(ex, game);
    // The seed goes in the replay IO buffer; the kernel copies it out before it
    // writes the replay integer back over the same bytes.
    const base = ex.wasm_replay_io_ptr();
    __mem(ex).set(seed, base);
    const n = ex.wasm_replay_encode_v6_from_game(maxAtoms);
    if (n < 0) throw __replayError(n, ex.wasm_replay_error_detail());
    return __mem(ex).slice(base, base + n);
}

// ---------------------------------------------------------------------------
// Replay steps (docs/C_CORE_CONSOLIDATION.md F4.2 / A5)
//
// A v6 code, rebuilt into the real Game and replayed through the real engine,
// handed back as the SAME packed evwire frames live play broadcasts. The caller
// decodes them with decodeEventWire — the one it already uses for live play —
// so a replay is not a second rendering path.
// ---------------------------------------------------------------------------

/** Steps a code replays to: the deal, then one per action. */
export function replayStepCount(code: Uint8Array): number {
    const ex = bots();
    __mem(ex).set(code, ex.wasm_replay_io_ptr());
    const n = ex.wasm_replay_step_count(code.length);
    if (n < 0) throw __replayError(n, ex.wasm_replay_error_detail());
    return n;
}

/**
 * Every evwire frame a code replays to, for `viewer` (-1 = spectator), in step
 * order. Pulled in chunks because the frames of a whole game — each carrying a
 * masked board snapshot — outgrow the wasm IO buffer, and because evwire's
 * n_events is a u8, so one frame per game is impossible regardless.
 */
export function replayEventFrames(code: Uint8Array, viewer: number): Uint8Array[] {
    const ex = bots();
    const frames: Uint8Array[] = [];
    const steps = replayStepCount(code);
    let from = 0;
    // Each chunk re-runs the replay from the start (the arithmetic decode is the
    // cost and it is ~1ms); a game takes a couple of chunks. The guard is a
    // no-progress backstop, not a step limit.
    for (let guard = 0; from < steps && guard < 4096; guard++) {
        __mem(ex).set(code, ex.wasm_replay_io_ptr());
        const len = ex.wasm_replay_events(viewer, from, code.length);
        if (len < 0) throw __replayError(len, ex.wasm_replay_error_detail());
        const next = ex.wasm_replay_events_next();
        if (next <= from) throw new Error(`replay frames stalled at step ${from}/${steps}`);
        const buf = __mem(ex);
        let q = ex.wasm_io_ptr();
        const end = q + len;
        for (let i = 0; i < ex.wasm_replay_events_n(); i++) {
            const flen = buf[q] | (buf[q + 1] << 8);
            q += 2;
            if (q + flen > end) throw new Error('replay frame ran past its chunk');
            frames.push(buf.slice(q, q + flen));
            q += flen;
        }
        from = next;
    }
    if (frames.length !== steps) {
        throw new Error(`replay produced ${frames.length} frames for ${steps} steps`);
    }
    return frames;
}
