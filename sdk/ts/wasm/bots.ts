// TypeScript bridge to the cnitro BOT module compiled to WebAssembly.
//
// bots.wasm is the rules kernel (same sources as rules.wasm) PLUS every
// algorithmic bot strategy (c/src/*_strategy.c) and a choose-move
// bridge (c/wasm/wasm_bots_api.c). The C sources are the single source
// of truth for bot play; this module only marshals the game in and reads the
// chosen move index out. The whole bot turn — legal-move enumeration,
// belief building, Monte-Carlo deliberation — runs inside the module.
//
// The chosen INDEX maps 1:1 onto the LegalMove list the server computed via
// kernelLegalMoves, because both come from the same C enumerator
// (c/src/legal.c calculate_legal_moves) over the same marshaled state.
//
// Loaded lazily and cached: the pure rules path (actions/) never pays for
// the larger module; the first bot decision instantiates it.

import { Card, Battle, Game } from '@api/core/types.ts';
import { LegalMove } from '@api/core/bot_interfaces.ts';
import { loadWasmGz, loadWasmGzAsync } from './wasm_asset.ts';

// view.h: mask every hand and the deck.
const VIEW_SPECTATOR = -1;
// view.c's wasm_view_serialize prefixes [format, viewer] before the state.
const VIEW_FORMAT_VERSION = 1;
import {
    EngineExports, PackedRunOk, __LOG_TYPE_TO_INT, __MOVE_TYPE, __adoptEngine,
    __marshalGame, __mem, __pooledCard, __replayError, __setResident,
    __wireLogCard, __cardFromWire, __wireStateCard, __residentLegalMoves, applyKernelStateToGame,
    exportPackedDriveProducts, rngBaseFromSeed, WIRE_NONE,
} from './engine.ts';

interface BotsExports extends EngineExports {
    wasm_import_logs(): void;
    wasm_replay_encode_v6_from_game(max_atoms: number): number;
    wasm_replay_events(viewer: number, from: number, code_len: number): number;
    wasm_replay_events_n(): number;
    wasm_replay_events_next(): number;
    wasm_replay_step_count(code_len: number): number;
    wasm_replay_step_index(code_len: number): number;
    wasm_view_json(len: number, viewer: number): number;
    wasm_events_json(len: number): number;
    wasm_unambiguous_cover(n_cover: number, n_battles: number, power_suit: number): number;
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
    // Animation core (c/src/anim_plan.h) — the platform-independent animation
    // policy the web pure modules (src/state/*) delegate to. bots-only.
    wasm_anim_should_drop_stale(hasLast: number, last: number, hasIncoming: number, incoming: number): number;
    wasm_anim_stale_optimistic(nOpt: number, nTable: number, nNamed: number): number;
    wasm_anim_resolve(nPending: number, nServer: number, nEvents: number,
                      defender: number, defenderHand: number, finalUncovered: number): number;
    wasm_anim_build_plan(nEvents: number, nPlayers: number, finalDeck: number, finalDiscard: number): number;
}

// Mirrors STRAT_* in c/src/strategy.h (only the ids the server uses).
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

/**
 * Prepare bots.wasm where it cannot be read synchronously — i.e. the browser,
 * which has no filesystem and must FETCH the .gz. Await this once before any
 * bots() call; on the server it is a no-op fast path (fs is synchronous).
 *
 * The browser needs the big module for two independent reasons, and both landed
 * the same day:
 *   * FMSG — the iMessage envelope — lives only here, because sealing reads a
 *     resident session log that rules.wasm structurally cannot hold, and /m/ is
 *     a web page; and
 *   * A5 — replaying a shared code rebuilds the game and plays it through the
 *     real engine, and replay_steps.c cannot live in rules.wasm either: its
 *     decoded-action buffer alone is ~272 KB against a linear memory PINNED at
 *     196,608 B.
 * One module behind every host is the steer (A10). This is the web's way in.
 *
 * Deliberately a fetched ASSET rather than a base64 twin of the same bytes: a
 * second carrier is exactly how rules_wasm.ts went stale for weeks while
 * bots.wasm.gz kept being rebuilt, leaving two kernels in the tree — and 80 KB
 * of single-line base64, rewritten on every kernel build, is miserable in git.
 * The cost is that the browser's door is async; callers await this first.
 */
export async function ensureBotsAsync(): Promise<void> {
    if (exportsCache) return;
    await loadWasmGzAsync('bots');   // caches the inflated bytes for bots()
    bots();
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
// (c/src/game.c random_strategy_random) with one fresh draw, so
// stochastic strategies stay stochastic across decisions. The parity/e2e
// harness can pin the stream (mirrors engine.ts __setKernelSeedSource).
let seedSource: (() => number) | null = null;
export function __setBotSeedSource(fn: (() => number) | null): void { seedSource = fn; }

// Session log marshal-in — wire layout of c/wasm/wasm_bots_api.c
// wasm_import_logs (u16 count; per log: i8 type, i8 player seat, i8
// defender_index, u8 num_pairs, num_pairs x (u8 primary, u8 target) 1-byte
// wire cards). Hidden cards travel as 0xFE ({-1,-1} in the log store) — the
// belief-based bots (cordite, fulminate) read these.
// The kernel STORES 1024 (MAX_LOGS in c/src/game.h) but ACCEPTS ~3072 raw
// records: a session log arrives untrimmed and the kernel filters dead goods
// itself. TS must not do that filtering — which side is allowed to decide that a
// good is dead is a rules question. Sized by the bots build's WASM_IO_CAP.
const MAX_KERNEL_LOGS = 3072;
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
// The knob VALUES are canonically the C roster's (c/src/bot_roster.c);
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

// BOT_STOP_* / BOT_PACE_* — c/src/bot_drive.h.
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

// ---------- FMSG: the iMessage envelope (c/src/msg_wire.h) -------------
//
// An iMessage game has no server: the whole game is (32-byte deal seed, v6
// replay code) in an MSMessage URL, and every device rebuilds it by re-dealing
// from the seed and replaying the code through the kernel. These two calls are
// the only way in — the envelope's layout lives in C, once, so the phone and
// the web can never read the same bytes as different games.
//
// They live HERE, on the big module, and not in engine.ts, because that is where
// the kernel puts them: sealing reads a resident session log, which rules.wasm
// structurally cannot hold (MAX_LOGS=128, no log import, 3-page pin). Splitting
// decode into the small module to dodge that would be a per-host kernel — the
// thing one-big-wasm exists to prevent. Calling either of these instantiates the
// big module and adopts the engine slot, so a caller cannot forget to.
//
// Both ride the REPLAY io buffer, never g_io. The rules build aliases the replay
// scratch family over the action family (CD_RULES_OVERLAY) on the grounds that
// the two never nest; an FMSG call IS a replay call (its body is a v6 code), so
// an envelope in g_io would be clobbered by the codec's own bignum scratch
// mid-decode. See wasm_api.c.

// The unpacked header — the private ABI msg_blob_write/msg_blob_read define in
// c/wasm/wasm_api.c. Fixed offsets, fixed-size join slots.
// Round 16 added the two send-clock bytes at 90, so the joins start at 92.
const MSG_BLOB_HDR = 93;
// 2 + the wire's MSG_MAX_NAME: was 14 (2 + 12) before round-5 B1 raised the
// name cap to 64 bytes (docs/APP_REVIEW_NOTES.md, c/src/msg_wire.h) — must
// match wasm_api.c's MSG_BLOB_JOIN or this bridge mis-parses every join.
const MSG_MAX_NAME = 64;
const MSG_BLOB_JOIN = 2 + MSG_MAX_NAME;

export interface MsgJoin { seat: number; name: string }

export interface MsgEnvelope {
    format: number;
    flags: number;
    phase: number;          // 0 WAITING, 1 ACCEPT, 2 LIVE, 3 FINISHED
    n_players: number;
    variant: number;
    round: number;          // completed bouts — Rule P's first key
    last_actor_seat: number;
    game_id: bigint;
    turn: number;           // atoms applied — Rule P's second key
    parent8: Uint8Array;    // first 8 bytes of SHA-256(parent envelope)
    seed: Uint8Array;       // 32
    digest: Uint8Array;     // SHA-256 of THIS envelope — Rule P's tiebreak
    /// ROUND 16: the send clock, unix seconds mod 65536, or 0 for a chain that
    /// carries none (format 2). Written as well as read: sealing with a
    /// non-zero clock is what makes an envelope format 3.
    sent_at: number;
    /// ROUND 16: the bubble delta - how many atoms THIS bubble added to the
    /// chain, or 0 for a chain that does not say (c/src/msg_wire.h). It is what
    /// tells a client to animate only the move it just opened instead of that
    /// move plus the one before it. READ-ONLY across this bridge: the kernel
    /// derives it at seal time from the chain it decoded, so anything written
    /// here is ignored (see wasm_api.c's blob layout).
    n_new: number;
    joins: MsgJoin[];
}

function msgError(code: number): Error {
    switch (code) {
        case -1: return new Error('iMessage payload: truncated');
        case -2: return new Error('iMessage payload: not an FMSG envelope');
        case -3: return new Error('iMessage payload: unsupported format');
        case -4: return new Error('iMessage payload: unsupported flags');
        case -5: return new Error('iMessage payload: bad phase');
        case -6: return new Error('iMessage payload: bad player count');
        case -7: return new Error('iMessage payload: unknown variant');
        case -8: return new Error('iMessage payload: bad seat');
        case -9: return new Error('iMessage payload: bad nickname');
        case -10: return new Error('iMessage payload: dead deal seed');
        case -11: return new Error('iMessage payload: malformed action');
        case -12: return new Error('iMessage payload: turn does not match the chain');
        case -13: return new Error('iMessage payload: round does not match the chain');
        case -14: return new Error('iMessage payload: capacity exceeded');
        case -15: return new Error('iMessage payload: trailing bytes');
        case -16: return new Error('iMessage payload: illegal chain');
        case -17: return new Error('iMessage payload: bad joins');
        case -18: return new Error('iMessage payload: body is not a replay code for this game');
        default: return new Error(`iMessage payload: kernel error ${code}`);
    }
}

function readBlob(buf: Uint8Array, base: number, len: number): MsgEnvelope {
    const b = buf.subarray(base, base + len);
    const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    const n_joins = b[7];
    const joins: MsgJoin[] = [];
    for (let i = 0; i < n_joins; i++) {
        const o = MSG_BLOB_HDR + i * MSG_BLOB_JOIN;
        const nameLen = b[o + 1];
        joins.push({
            seat: b[o],
            name: new TextDecoder().decode(b.subarray(o + 2, o + 2 + nameLen)),
        });
    }
    return {
        format: b[0], flags: b[1], phase: b[2], n_players: b[3],
        variant: b[4], round: b[5], last_actor_seat: b[6],
        game_id: dv.getBigUint64(8, true),
        turn: dv.getUint16(16, true),
        parent8: b.slice(18, 26),
        seed: b.slice(26, 58),
        digest: b.slice(58, 90),
        sent_at: dv.getUint16(90, true),
        n_new: b[92],
        joins,
    };
}

function writeBlob(e: MsgEnvelope): Uint8Array {
    const out = new Uint8Array(MSG_BLOB_HDR + e.joins.length * MSG_BLOB_JOIN);
    const dv = new DataView(out.buffer);
    out[0] = e.format; out[1] = e.flags; out[2] = e.phase; out[3] = e.n_players;
    out[4] = e.variant; out[5] = e.round; out[6] = e.last_actor_seat;
    out[7] = e.joins.length;
    dv.setBigUint64(8, e.game_id, true);
    dv.setUint16(16, e.turn, true);
    out.set(e.parent8.subarray(0, 8), 18);
    out.set(e.seed.subarray(0, 32), 26);
    // 58..90 is the digest: decode-only (an envelope cannot carry its own).
    dv.setUint16(90, e.sent_at & 0xffff, true);
    // 92 is n_new: decode-only too (msg_seal derives the delta, see bots.ts's
    // MsgEnvelope.n_new), so it is left 0 here rather than echoed back.
    e.joins.forEach((j, i) => {
        const o = MSG_BLOB_HDR + i * MSG_BLOB_JOIN;
        const name = new TextEncoder().encode(j.name);
        if (name.length > MSG_MAX_NAME) throw new Error(`nickname over ${MSG_MAX_NAME} bytes: ${j.name}`);
        out[o] = j.seat;
        out[o + 1] = name.length;
        out.set(name, o + 2);
    });
    return out;
}

// Decode + VALIDATE an envelope: the chain is replayed through the kernel, so a
// corrupt or hand-edited payload throws rather than half-loading (§7.3 —
// validation IS replay). Leaves the game RESIDENT: kernelViewSerialize and the
// legal-move exports then read exactly the state the payload describes, which is
// what the /m/ route renders and what a turn continues from.
export function kernelMsgDecode(envelope: Uint8Array): MsgEnvelope {
    const ex = bots();
    if (!ex.wasm_msg_decode) throw new Error('kernelMsgDecode: module has no FMSG support');
    if (envelope.length > ex.wasm_replay_io_cap()) throw new Error('iMessage payload: capacity exceeded');
    const base = ex.wasm_replay_io_ptr();
    __mem(ex).set(envelope, base);
    const r = ex.wasm_msg_decode(envelope.length);
    if (r < 0) throw msgError(r);
    // The chain replayed into the kernel's resident game; nothing may assume a
    // previously-marshalled object still describes it.
    __setResident(null);
    return readBlob(__mem(ex), base, r);
}

// Seal the RESIDENT game into an envelope. `header` supplies what the protocol
// owns (game_id, phase, seed, joins, parent8, last_actor_seat); the kernel fills
// in what the BODY owns — turn, round — by decoding the code it just wrote, so a
// host cannot emit a payload it would itself reject. Returns the wire bytes.
//
// Rule P (§7.2): which chain does every device prefer? <0 a, >0 b, 0 the same.
//
// The comparison is in C (msg_rule_p) and not here, deliberately. This decides
// which game every player sees; a browser and a phone disagreeing forks the
// game, so there is one implementation and nothing to port to Swift. TS only
// moves bytes.
export function kernelMsgRuleP(a: Uint8Array, b: Uint8Array): number {
    const ex = bots();
    if (!ex.wasm_msg_rule_p) throw new Error('kernelMsgRuleP: module has no FMSG support');
    if (a.length + b.length > ex.wasm_replay_io_cap()) throw new Error('iMessage payload: capacity exceeded');
    const base = ex.wasm_replay_io_ptr();
    __mem(ex).set(a, base);
    __mem(ex).set(b, base + a.length);
    const r = ex.wasm_msg_rule_p(a.length, b.length);
    // -1/0/+1 are verdicts; anything below is a decode error (MSG_E* < -1).
    if (r < -1) throw msgError(r);
    return r;
}

// The legal moves of the game the last kernelMsgDecode adopted — read from the
// kernel, never marshalled. An iMessage device does not hold the game as a TS
// object: the envelope put it in the kernel, and that is the only copy.
//
// This is how the extension/route answers "what can I do?" — never by asking TS.
// A hand-rolled "is it my turn" is a bug by policy (design §17.16).
export function kernelMsgLegalMoves(seat: number): { type: string; cards?: Card[]; attack_cards?: Card[] }[] {
    return __residentLegalMoves(bots() as unknown as EngineExports, seat);
}

export const MSG_REBASE_REAPPLY = 0;
export const MSG_REBASE_DISCARD_ROUND = 1;
export const MSG_REBASE_DISCARD_ILLEGAL = 2;

// Rule R (§7.4): rebase ONE pending action onto the chain kernelMsgDecode last
// adopted, in ledger order. REAPPLY mutates the resident game — that IS the
// rebase — so keep folding the rest in after it; a DISCARD leaves it untouched.
//
// The round-boundary guard and the legality question both live in C. A throw-in
// composed against round 5, after a pickup closed round 5, would re-validate as
// an opening attack of round 6 — legal, and not what the player chose.
export function kernelMsgRebase(pendingRound: number, seat: number, wire: Uint8Array): number {
    const ex = bots();
    if (!ex.wasm_msg_rebase) throw new Error('kernelMsgRebase: module has no FMSG support');
    if (wire.length > 128) throw new Error('malformed action wire');
    __mem(ex).set(wire, ex.wasm_cards_a_ptr());
    const r = ex.wasm_msg_rebase(pendingRound, seat, wire.length);
    if (r < 0) throw msgError(r);
    __setResident(null);
    return r;
}

// ---------------------------------------------------------------------------
// One-tap cover resolution (A7/F9)
//
// The one-gesture cover affordance, decided in the kernel (legal.c's
// unambiguous_cover) so the web drag, phone tap-commit, watch chooser and
// iMessage share one resolver instead of a coverCombinations.ts copy each.
// Reads only the handful of cards passed — no game marshal — so it is cheap
// enough to call while a drag hovers. Runs on the browser's warm bots.wasm
// (the A8 KernelGate guarantees it is loaded before any board renders).
// ---------------------------------------------------------------------------

/** The paired result: cover card i defends attackCards[i]. Mirrors the shape the
 * deleted coverCombinations.ts findUnambiguousCover returned. */
export interface CoverCombination { coverCards: Card[]; attackCards: Card[]; }

/**
 * If `coverCards` cover the table's uncovered attacks in exactly one unambiguous
 * way (every valid full pairing covers the same set of attacks), return that
 * pairing; otherwise null (the UI then lets the player place cards manually).
 */
export function kernelUnambiguousCover(
    coverCards: Card[], tableBattles: Battle[], powerSuit: number,
): CoverCombination | null {
    if (coverCards.length === 0) return null;
    const ex = bots();
    const mem = __mem(ex);
    const aptr = ex.wasm_cards_a_ptr();
    for (let i = 0; i < coverCards.length; i++) mem[aptr + i] = __wireLogCard(coverCards[i]);
    const bptr = ex.wasm_cards_b_ptr();
    for (let i = 0; i < tableBattles.length; i++) {
        mem[bptr + 2 * i] = __wireLogCard(tableBattles[i].attack);
        mem[bptr + 2 * i + 1] = tableBattles[i].defense ? __wireLogCard(tableBattles[i].defense) : WIRE_NONE;
    }
    const n = ex.wasm_unambiguous_cover(coverCards.length, tableBattles.length, powerSuit);
    if (n <= 0) return null;
    // Re-fetch the memory view: a wasm call can grow (and detach) the buffer.
    const out = __mem(ex);
    const io = ex.wasm_io_ptr();
    const attackCards: Card[] = [];
    for (let i = 0; i < n; i++) attackCards.push(__cardFromWire(out[io + i]));
    return { coverCards: [...coverCards], attackCards };
}

// The PUBLIC view of the game the last kernelMsgDecode adopted — every hand as
// backs, the deck masked. What /m/ renders for a stranger with a link, and what
// the bubble snapshot shows (it lands in notifications and on lock screens, so
// it must never carry a hand — design §5 invariants).
//
// wasm_view_serialize reads the RESIDENT game, so this needs no re-deserialize:
// the envelope already put the game in the kernel. The masking itself is in
// view.c, like every other view in the product — nothing here decides what a
// stranger may see.
export function kernelMsgPublicView(): { view: KernelState } {
    const ex = bots() as unknown as EngineExports;
    const base = ex.wasm_io_ptr();
    const len = ex.wasm_view_serialize(VIEW_SPECTATOR);
    const blob = __mem(ex).slice(base, base + len);
    // The blob leads with [VIEW_FORMAT_VERSION, viewer] (wasm_view_serialize);
    // the masked state starts after it.
    if (blob.length < 2) throw new Error('view: empty payload');
    if (blob[0] !== VIEW_FORMAT_VERSION) {
        throw new Error(`view: format ${blob[0]}, this build reads ${VIEW_FORMAT_VERSION}`);
    }
    // Serialized by the kernel, read back by the kernel. It used to hand the
    // blob to a TS parser, which meant this one call crossed the wire format
    // twice in two different implementations of it.
    return { view: kernelViewFromPacked(blob.subarray(2), VIEW_SPECTATOR) };
}

// ---------------------------------------------------------------------------
// Packed bytes -> objects, decoded by the kernel (A8/F7)
//
// The door the web's wire decode goes through now. The TS that used to read
// these formats shadowed view.c and evwire.c byte for byte and was kept true by
// a parity test; the layout lives in C alone now, and this asks it for objects.
// iOS has always worked this way — src/json_out.c is literally the same code.
//
// What comes back is RAW: ints where the kernel has ints, seats where the
// kernel has seats, null for a masked card. It is not the view model. Identity
// (player_id/name/is_ai), good-order, timestamps and message prose are joined on
// afterwards by the host, because the kernel does not have them and says so —
// see wire/view.ts's viewToGame, which is where that join stayed.
//
// Buffer discipline mirrors the kernel side: the packed input goes to the REPLAY
// io buffer, the JSON comes back in the MAIN one, so the two never alias.
// ---------------------------------------------------------------------------

// json_out.h's negative returns.
const JSON_EBADARG = -1, JSON_ECAP = -3, JSON_EPARSE = -4;

function __jsonError(code: number, what: string): Error {
    switch (code) {
        case JSON_EBADARG: return new Error(`${what}: bad argument (empty payload, or a viewer seat off the board)`);
        case JSON_ECAP:    return new Error(`${what}: decoded JSON exceeds the kernel IO buffer`);
        case JSON_EPARSE:  return new Error(`${what}: not a readable payload (truncated, or a format this build does not read)`);
        default:           return new Error(`${what}: kernel error ${code}`);
    }
}

function __jsonCall(bytes: Uint8Array, what: string, run: (ex: BotsExports) => number): unknown {
    const ex = bots();
    __mem(ex).set(bytes, ex.wasm_replay_io_ptr());
    const len = run(ex);
    if (len < 0) throw __jsonError(len, what);
    const base = ex.wasm_io_ptr();
    // subarray, not slice: decoded and handed to JSON.parse immediately, before
    // any other kernel call can touch the buffer.
    return JSON.parse(new TextDecoder().decode(__mem(ex).subarray(base, base + len)));
}

/** One seat's masked hand as the kernel emits it — null entries are card backs. */
export interface KernelCard { s: number; v: number }

export interface KernelPlayerState {
    seat: number; name: string; status: number; handCount: number;
    awaitingAttack: boolean; strategyKey: number;
    hand: KernelCard[] | null;   // null for every seat that is not the viewer
}

/** A viewer-masked board, exactly as src/json_out.c's json_state writes it. */
export interface KernelState {
    status: number; numPlayers: number; powerSuit: number;
    deckCount: number; discardCount: number; hasFlipped: boolean;
    firstAttacker: number; defender: number; viewer: number;
    goodMask: number; hasGoodTs: boolean; gameOver: number;
    flipped: KernelCard | null;
    battles: { attack: KernelCard; defense: KernelCard | null }[];
    eliminationOrder: number[];
    players: KernelPlayerState[];
}

export interface KernelEvent {
    type: number; seat: number; msg: number; from: number; to: number;
    cards: (KernelCard | null)[];
    target?: KernelCard;
    battle?: number;
    state: KernelState;
}

export interface KernelSequence {
    viewer: number; actor: number;
    events: KernelEvent[];
    game: KernelState;
}

/**
 * Decode a packed masked view blob (view.c's state_get layout) into the board it
 * describes. `viewer` is the seat whose hand is real in the blob, or -1 for the
 * spectator feed. Throws on an unreadable payload — never returns a partial board.
 */
export function kernelViewFromPacked(blob: Uint8Array, viewer: number): KernelState {
    return __jsonCall(blob, 'kernelViewFromPacked',
                      ex => ex.wasm_view_json(blob.length, viewer)) as KernelState;
}

/**
 * Decode a packed evwire sequence (the bytes live play broadcasts and a replay
 * frame carries) into its events, each with the board as of that step.
 */
export function kernelEventsFromPacked(bytes: Uint8Array): KernelSequence {
    return __jsonCall(bytes, 'kernelEventsFromPacked',
                      ex => ex.wasm_events_json(bytes.length)) as KernelSequence;
}

/**
 * ROUND 16 - how many seconds this seat must still wait before it may pick up,
 * against the game the last `kernelMsgDecode` left resident. 0 means now.
 *
 * `sentAt` is the decoded envelope's `sent_at` and `now` the caller's own unix
 * seconds mod 65536; a chain with no clock (format 2) always answers 0, which
 * is what keeps every bubble sealed by a shipped build playable.
 */
export function kernelMsgPickupHold(seat: number, sentAt: number, now: number): number {
    const ex = bots();
    if (!ex.wasm_msg_pickup_hold) throw new Error('kernelMsgPickupHold: module has no FMSG support');
    return ex.wasm_msg_pickup_hold(seat, sentAt & 0xffff, now & 0xffff);
}

export function kernelMsgSeal(
    header: Omit<MsgEnvelope, 'digest' | 'turn' | 'round' | 'format' | 'sent_at' | 'n_new'>
          & { sent_at?: number },
): Uint8Array {
    const ex = bots();
    if (!ex.wasm_msg_seal) throw new Error('kernelMsgSeal: module has no FMSG support');
    // format 2 here is a placeholder: msg_seal picks the real one off what the
    // header ends up carrying (a clock, or a bubble delta, seals format 3), so
    // the caller never states it twice. n_new is 0 for the same reason the
    // digest is: the kernel derives it, from the chain wasm_msg_decode adopted.
    const blob = writeBlob({ ...header, sent_at: header.sent_at ?? 0, n_new: 0,
                             format: 2, turn: 0, round: 0, digest: new Uint8Array(32) });
    const base = ex.wasm_replay_io_ptr();
    __mem(ex).set(blob, base);
    const r = ex.wasm_msg_seal(blob.length);
    if (r < 0) throw msgError(r);
    return __mem(ex).slice(base, base + r);
}

// The best shareable REPLAY code for the game the last kernelMsgDecode
// adopted — the TS-side twin of MessageKernel.residentReplayCode()
// (sdk/swift/MessageEnvelope.swift), reached off the same resident g_game
// instead of Swift's fio_replay_share_code_b32. Used by the /m/ page's
// FINISHED-bubble funnel (docs/IMESSAGE_LOBBY_V2.md, batch 6 item B): once a
// payload decodes, msg_replay has already run the whole chain through the
// ORDINARY kernel handlers (handle_attack etc.), which log exactly like any
// other play — so the resident game already carries the full session log a
// v6 code needs. Only the envelope's own seed (env.seed, already decoded — no
// second kernel round-trip to fetch it) has to be supplied.
//
// Deliberately NOT kernelReplayEncodeV6FromGame: that helper re-marshals a
// `Game` object (__marshalGame -> wasm_import_state), which resets the
// resident log to 0 and would throw away exactly the log kernelMsgDecode just
// built. This calls the same C export (wasm_replay_encode_v6_from_game)
// directly against whatever is already resident, exactly as fio_replay_share_
// code_b32 does on the native/Swift side.
export function kernelResidentReplayCodeV6(seed: Uint8Array): Uint8Array {
    if (seed.length !== 32) {
        throw new Error(`replay: v6 needs a 32-byte deal seed, got ${seed.length}`);
    }
    const ex = bots();
    const base = ex.wasm_replay_io_ptr();
    __mem(ex).set(seed, base);
    const n = ex.wasm_replay_encode_v6_from_game(1 << 30);
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

/** REPLAY_ATOM_* — what a step played. Mirrors c/src/replay.h. */
export const REPLAY_STEP = {
    DEAL: 0,        // the opening deal; never an action
    DRAW: 1,        // never a step (draws ride the action that caused them)
    ATTACK: 2,
    COVER: 3,
    PASS: 4,
    PICKUP: 5,
    ROUND_END: 6,   // every remaining attacker said good and the bout closed
    GOOD: 7,        // one seat said good and the bout stayed open
} as const;

export const REPLAY_STEP_SEAT_NONE = 0xff;

export interface ReplayStepInfo {
    /** REPLAY_STEP.* — the action this step played. */
    kind: number;
    /** The acting seat, or -1 (the deal, and round ends nobody in particular closes). */
    seat: number;
}

/**
 * What each step of a code IS, in step order. The kernel reports this rather
 * than the web inferring it from the frames, because the frames genuinely
 * cannot say: an attack and a pass are the same evwire event type, told apart
 * only by a reconstructed English message. See replay_steps.h.
 */
export function replayStepIndex(code: Uint8Array): ReplayStepInfo[] {
    const ex = bots();
    __mem(ex).set(code, ex.wasm_replay_io_ptr());
    const len = ex.wasm_replay_step_index(code.length);
    if (len < 0) throw __replayError(len, ex.wasm_replay_error_detail());
    const buf = __mem(ex);
    const base = ex.wasm_io_ptr();
    const out: ReplayStepInfo[] = [];
    for (let q = base; q < base + len; q += 2) {
        const seat = buf[q + 1];
        out.push({ kind: buf[q], seat: seat === REPLAY_STEP_SEAT_NONE ? -1 : seat });
    }
    return out;
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

// ===========================================================================
// Animation core (c/src/anim_plan.h)
// ===========================================================================
// The bridge the web pure modules (src/state/*) delegate to, so the animation
// policy the React glitch-fixing hardened lives in C once — the same "one kernel
// behind every host" argument FMSG makes. bots.wasm is loaded at app boot
// (providers.tsx awaits ensureBotsAsync), and bots() is synchronous on the
// server, so these calls are safe synchronously in both.

// ANIM_EVT_* — mirrors anim_plan.h (which mirrors ANIMATION_EVENT_TYPE / EVW_T_*).
export const ANIM_EVT: Record<string, number> = {
    magic_transition: 0, deal: 1, flipped: 2, defender_move: 3, attack_pass: 4,
    cover: 5, pickup: 6, discard: 7, out: 8, refill: 9, cards_to_trash: 10, revert: 11,
};
// ANIM_LOC_* — mirrors anim_plan.h.
export const ANIM_LOC: Record<string, number> = {
    deck: 0, hand: 1, table: 2, discard: 3, flipped: 4,
};
const ANIM_LOC_NONE = 0xff;

/** The event-type string -> ANIM_EVT_* code (0 for an unknown/None type). */
export function animEventTypeCode(type: string | undefined): number {
    return (type && type in ANIM_EVT) ? ANIM_EVT[type] : 0;
}

/** clientReconcile.shouldDropStaleSequence, in C. null models "no version"
 *  (a replay sequence, never gated). */
export function animShouldDropStale(last: number | null, incoming: number | null): boolean {
    const ex = bots();
    return ex.wasm_anim_should_drop_stale(
        last === null ? 0 : 1, last ?? 0,
        incoming === null ? 0 : 1, incoming ?? 0) !== 0;
}

/** optimisticAnimation.staleOptimisticKeysOnTable, in C. Returns the INDICES
 *  into `optCards` to release. */
export function animStaleOptimisticOnTable(optCards: Card[], tableCards: Card[], namedCards: Card[]): number[] {
    const ex = bots();
    if (optCards.length > 128 || tableCards.length > 160 || namedCards.length > 160) {
        throw new Error('anim: card list exceeds ABI cap');
    }
    const buf = __mem(ex);
    const base = ex.wasm_io_ptr();
    let p = base;
    for (const c of optCards) buf[p++] = __wireStateCard(c);
    for (const c of tableCards) buf[p++] = __wireStateCard(c);
    for (const c of namedCards) buf[p++] = __wireStateCard(c);
    const n = ex.wasm_anim_stale_optimistic(optCards.length, tableCards.length, namedCards.length);
    if (n < 0) throw new Error(`anim_stale_optimistic error ${n}`);
    const out = __mem(ex);
    const ob = ex.wasm_io_ptr();
    const rel: number[] = [];
    for (let i = 0; i < n; i++) rel.push(out[ob + i]);
    return rel;
}

/** optimisticConflicts.resolveUnconfirmedAttackCovers, in C. `events` need only
 *  carry a type code (animEventTypeCode) and the cards each names — the C side
 *  uses them for the pickup/cards_to_trash sweep set. Returns index lists into
 *  `pending`. */
export function animResolveUnconfirmed(
    pending: { card: Card; isCover: boolean }[],
    serverTable: Card[],
    events: { type: number; cards: Card[] }[],
    fin: { defender: number; defenderHand: number; finalUncovered: number },
): { revert: number[]; merge: number[]; clear: number[] } {
    const ex = bots();
    if (pending.length > 128 || serverTable.length > 160 || events.length > 64) {
        throw new Error('anim: resolve input exceeds ABI cap');
    }
    const buf = __mem(ex);
    const base = ex.wasm_io_ptr();
    let p = base;
    for (const pc of pending) { buf[p++] = __wireStateCard(pc.card); buf[p++] = pc.isCover ? 1 : 0; }
    for (const c of serverTable) buf[p++] = __wireStateCard(c);
    for (const e of events) {
        buf[p++] = e.type & 0xff;
        buf[p++] = e.cards.length & 0xff;
        for (const c of e.cards) buf[p++] = __wireStateCard(c);
    }
    const rc = ex.wasm_anim_resolve(pending.length, serverTable.length, events.length,
                                    fin.defender, fin.defenderHand, fin.finalUncovered);
    if (rc < 0) throw new Error(`anim_resolve error ${rc}`);
    const out = __mem(ex);
    let q = ex.wasm_io_ptr();
    const nRevert = out[q++], nMerge = out[q++], nClear = out[q++];
    const revert: number[] = [], merge: number[] = [], clear: number[] = [];
    for (let i = 0; i < nRevert; i++) revert.push(out[q++]);
    for (let i = 0; i < nMerge; i++) merge.push(out[q++]);
    for (let i = 0; i < nClear; i++) clear.push(out[q++]);
    return { revert, merge, clear };
}

// One built plan step (mirrors AnimPlanStep).
export interface AnimPlanStep {
    type: number; seat: number; from: number; to: number; nCards: number;
    durationMs: number; startMs: number; deck: number; discard: number;
    inFlightFromDeck: number; inFlightToFlipped: number; hand: number[];
}
export interface AnimPlan {
    nSteps: number; nPlayers: number;
    pre: { deck: number; discard: number; hand: number[] };
    totalMs: number; veilIds: number[]; steps: AnimPlanStep[];
}

/** anim_build_plan, in C: a decoded viewer sequence -> the timed plan (count-
 *  freeze + veil + durations). Provided for iOS/Steam parity and completeness;
 *  the web's React queue currently renders its own pacing (a TODO seam — see
 *  docs/ANIMATION_CORE_C.md). `events[].seat` may be null for a seat-less event. */
export function animBuildPlan(
    events: { type: number; seat: number | null; from: number; to: number; mask: boolean; cards: Card[] }[],
    nPlayers: number, finalDeck: number, finalDiscard: number, finalHand: number[],
): AnimPlan {
    const ex = bots();
    if (events.length > 64) throw new Error('anim: plan exceeds ABI cap');
    const buf = __mem(ex);
    const base = ex.wasm_io_ptr();
    let p = base;
    for (let s = 0; s < nPlayers; s++) buf[p++] = finalHand[s] & 0xff;
    for (const e of events) {
        buf[p++] = e.type & 0xff;
        buf[p++] = e.seat === null ? ANIM_LOC_NONE : (e.seat & 0xff);
        buf[p++] = e.from & 0xff;
        buf[p++] = e.to & 0xff;
        buf[p++] = e.mask ? 1 : 0;
        buf[p++] = e.cards.length & 0xff;
        for (const c of e.cards) buf[p++] = __wireStateCard(c);
    }
    const len = ex.wasm_anim_build_plan(events.length, nPlayers, finalDeck, finalDiscard);
    if (len < 0) throw new Error(`anim_build_plan error ${len}`);
    const out = __mem(ex);
    let q = ex.wasm_io_ptr();
    const rd16 = () => { const v = out[q] | (out[q + 1] << 8); q += 2; return v; };
    const nSteps = out[q++];
    const np = out[q++];
    const preDeck = rd16(), preDiscard = rd16();
    const preHand: number[] = [];
    for (let s = 0; s < np; s++) preHand.push(rd16());
    const totalMs = rd16();
    const nVeil = out[q++];
    const veilIds: number[] = [];
    for (let i = 0; i < nVeil; i++) veilIds.push(out[q++]);
    const steps: AnimPlanStep[] = [];
    for (let i = 0; i < nSteps; i++) {
        const type = out[q++], seat = out[q++], from = out[q++], to = out[q++], nCards = out[q++];
        const durationMs = rd16(), startMs = rd16(), deck = rd16(), discard = rd16();
        const inFlightFromDeck = out[q++], inFlightToFlipped = out[q++];
        const hand: number[] = [];
        for (let s = 0; s < np; s++) hand.push(rd16());
        steps.push({ type, seat: seat === 0xff ? -1 : seat, from, to, nCards,
                     durationMs, startMs, deck, discard, inFlightFromDeck, inFlightToFlipped, hand });
    }
    return { nSteps, nPlayers: np, pre: { deck: preDeck, discard: preDiscard, hand: preHand }, totalMs, veilIds, steps };
}
