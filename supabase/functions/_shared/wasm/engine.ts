// TypeScript bridge to the cnitro rules kernel compiled to WebAssembly.
//
// The C engine (cnitro/src/game.c + legal.c) is the single source of truth
// for game rules. This module marshals the TS `Game` object into the
// kernel's byte layout, runs the action, and reconstructs everything the
// production server used to compute in TS:
//   - the mutated Game (hands, table, deck, elimination, defender rotation)
//   - the GameLog stream (appended to game.logs, DRAW identities hidden
//     exactly like the TS engine did)
//   - the AnimationEvent stream, including the per-step intermediate
//     game_state snapshots (the kernel fires engine_snap_hook at exactly the
//     points where the TS handlers called cloneGame)
//   - the production error messages on rejection (the kernel reports a
//     reason code; the strings here are presentation only)
//
// The wasm module is embedded as base64 (rules_wasm.ts) so Deno edge
// functions, Node (tsx/e2e) and browsers all load it with zero asset
// plumbing. Instantiation is synchronous — the module is 27 KB and has no
// imports. (Browsers cap synchronous compilation on the main thread; no
// client code imports this module today. If that changes, add an async
// preload path.)

import {
    Card, Game, PrivatePlayer, Battle, GameLog, AnimationEvent,
    ANIMATION_EVENT_TYPE, LOG_TYPE, LogType, GAME_STATUS, PLAYER_STATUS,
} from '../types.ts';
import { VALUE_MAP, SUIT_MAP } from '../constants.ts';
import { takeRULES_WASM_B64 } from './rules_wasm.ts';
// The rules embed is gzip+base64 (embed.mjs --gzip). gunzip is a vendored
// pure-JS + SYNCHRONOUS inflate (relative import), so it works in the browser
// (unlike node:zlib), keeps engine()'s sync instantiate (unlike async
// DecompressionStream), and needs no npm/import-map on the Deno edge.
import { gunzip } from './gunzip.ts';

// ---------------------------------------------------------------------------
// Instantiation
// ---------------------------------------------------------------------------

interface EngineExports {
    memory: WebAssembly.Memory;
    wasm_init(): void;
    wasm_set_seed(s: number): void;
    wasm_set_deal_seed_bytes(): void;
    wasm_reject_reason(): number;
    wasm_io_ptr(): number;
    wasm_io_cap(): number;
    wasm_cards_a_ptr(): number;
    wasm_cards_b_ptr(): number;
    wasm_import_state(): void;
    wasm_set_deterministic_deck(on: number): void;
    wasm_seed_rng_deterministic(): void;
    wasm_set_strategy_seed_deterministic(): void;
    wasm_set_rng_base(base: number): void;
    wasm_export_state(): number;
    wasm_state_serialize(): number;
    wasm_state_deserialize(len: number): number;
    wasm_state_format_version(): number;
    wasm_export_logs(): number;
    wasm_snap_count(): number;
    wasm_snap_tag(i: number): number;
    wasm_snap_aux(i: number): number;
    wasm_export_snapshot(i: number): number;
    wasm_start_game(): number;
    wasm_attack(p: number, n: number): number;
    wasm_cover(p: number, n: number): number;
    wasm_pass(p: number, n: number): number;
    wasm_pickup(p: number): number;
    wasm_good(p: number): number;
    wasm_transition(): number;
    wasm_refill(): number;
    wasm_game_done(): number;
    wasm_should_act(i: number): number;
    wasm_next_player(cur: number): number;
    wasm_can_cover(as: number, av: number, ds: number, dv: number, ps: number): number;
    wasm_legal_moves(i: number): number;
    wasm_export_moves(start: number, max: number): number;
    wasm_replay_io_ptr(): number;
    wasm_replay_io_cap(): number;
    wasm_replay_encode(len: number): number;
    wasm_replay_decode(len: number): number;
    wasm_replay_error_detail(): number;
    // Packed wire pipeline (docs/PACKED_WIRE_CUTOVER.md)
    wasm_export_logs_masked(): number;
    wasm_apply_action(seat: number, wireLen: number): number;
    wasm_finalize_win(aiMask: number): number;
    wasm_view_serialize(viewer: number): number;
    wasm_events_serialize(viewer: number, actor: number, ended: number): number;
    wasm_rearrange_hand(seat: number, n: number): number;
}

function decodeBase64(b64: string): Uint8Array {
    if (typeof atob === 'function') {
        const bin = atob(b64);
        const out = new Uint8Array(bin.length);
        for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
        return out;
    }
    // Node without atob (older runtimes)
    // deno-lint-ignore no-explicit-any
    return new Uint8Array((globalThis as any).Buffer.from(b64, 'base64'));
}

let exportsCache: EngineExports | null = null;

// Memory diagnostics for the edge 150MB-external budget (see utils.ts [MEM]
// logging): current size of the resident kernel's linear memory, -1 until
// loaded. After __adoptEngine this reports the bots.wasm memory.
export function __kernelWasmMB(): number {
    const mem = exportsCache?.memory;
    return mem ? Math.round(mem.buffer.byteLength / 1048576) : -1;
}

// Raw linear-memory bytes (wasm grows in 64KB pages, so this is page-granular) —
// finer than the MB round above for benchmarks that need to see small deltas.
export function __kernelWasmBytes(): number {
    const mem = exportsCache?.memory;
    return mem ? mem.buffer.byteLength : -1;
}

// The embed is take-once (a second take throws — see cnitro/wasm/embed.mjs),
// so the decoded bytes are held here from first take until an instantiation
// SUCCEEDS: a failed attempt (or a sync call racing an in-flight async one)
// stays retryable instead of hitting 'already taken'.
let pendingWasmBytes: Uint8Array | null = null;
function rulesWasmBytes(): Uint8Array {
    if (!pendingWasmBytes) pendingWasmBytes = gunzip(decodeBase64(takeRULES_WASM_B64()));
    return pendingWasmBytes;
}

function engine(): EngineExports {
    if (exportsCache) return exportsCache;
    // [perf] one-time per isolate: gunzip embed + compile + instantiate + init.
    // Isolates recycle often on the edge, so this is paid on the first kernel
    // call of most requests — worth knowing if it dominates request latency.
    const t = performance.now();
    const module = new WebAssembly.Module(rulesWasmBytes() as BufferSource);
    const instance = new WebAssembly.Instance(module, {});
    const ex = instance.exports as unknown as EngineExports;
    ex.wasm_init();
    exportsCache = ex;
    pendingWasmBytes = null;
    console.log(`[perf] rules.wasm instantiate ${(performance.now() - t).toFixed(0)}ms`);
    return ex;
}

// Async preload — the path browser code must take before any kernel call:
// Chrome caps synchronous WebAssembly compilation on the main thread at a
// few KB, far below this module. Server runtimes (Deno edge, Node tests)
// keep the sync engine() they always used; the replay decode path awaits
// this first so the SAME decode code runs on both sides of the wire.
let enginePromise: Promise<void> | null = null;
export function ensureEngineAsync(): Promise<void> {
    if (exportsCache) return Promise.resolve();
    if (typeof document === 'undefined') {
        // Not a browser main thread: the sync path is allowed and cheaper.
        engine();
        return Promise.resolve();
    }
    if (!enginePromise) {
        enginePromise = WebAssembly
            .instantiate(rulesWasmBytes() as BufferSource, {})
            .then(({ instance }) => {
                // bots.wasm may have adopted the engine slot while this was
                // in flight — never clobber a live instance (its kernel may
                // hold resident state the bot loop is about to consume).
                if (exportsCache) return;
                const ex = instance.exports as unknown as EngineExports;
                ex.wasm_init();
                exportsCache = ex;
                memView = new Uint8Array(0);
                residentFor = null;
                pendingWasmBytes = null;
            })
            .catch((e) => {
                enginePromise = null; // retryable: bytes are still cached
                throw e;
            });
    }
    return enginePromise;
}

// The bots module (bots.wasm) embeds this same kernel plus the strategies.
// When it loads, it adopts the engine slot so choose + action run on ONE
// instance — which enables the resident-state fast path below.
export function __adoptEngine(ex: EngineExports): void {
    exportsCache = ex;
    memView = new Uint8Array(0);
    residentFor = null;
}

// Resident-state fast path: wasmChooseMove marshals the game and only READS
// it; the bot loop then immediately executes the chosen action on the same
// game object. Marking the object resident lets that next marshal be
// skipped — the kernel state is byte-identical to what a fresh marshal
// would rebuild (choose is read-only; imported logs are cleared kernel-side
// so the action logs from zero exactly like after wasm_import_state).
//
// SAFETY: the mark is only valid if the object is UNCHANGED between the choose
// and the action. The bot loop, however, reuses one game object and can mutate
// it between decisions (state reload on a CAS conflict, refill, passive
// bundling). So the skip must be consumed ONLY by the action executing the
// just-chosen move: every state READER (wasmChooseMove, kernelLegalMoves)
// clears the mark before marshaling and thus always rebuilds from the live
// object. ANY marshal also clears the mark, keeping the window to a single
// kernel call.
let residentFor: Game | null = null;
export function __setResident(g: Game | null): void { residentFor = g; }

// One cached view over the whole linear memory. The kernel never mallocs, so
// memory.grow is never called and the buffer identity is stable — but guard
// anyway. Reading through one persistent view (with explicit base offsets)
// avoids allocating a subarray per call, which showed up as GC pressure in
// profiles.
let memView = new Uint8Array(0);
function mem(ex: EngineExports): Uint8Array {
    if (memView.buffer !== ex.memory.buffer) memView = new Uint8Array(ex.memory.buffer);
    return memView;
}

// Sign-fix for i8 fields read through the u8 view. Module-scoped: defining
// helpers inside the hot parse functions made the bundler's keepNames
// wrapper (__name) a measurable per-call cost.
const i8 = (v: number) => (v > 127 ? v - 256 : v);
const MOVE_TYPE = ['attack', 'cover', 'pass', 'pickup', 'good', 'wait'];

// Interned card objects. Every deck/hand/battle/log/move parse used to
// allocate fresh {suit,value} objects — tens of thousands per second of pure
// GC food. There are only 52 cards (plus the hidden-card sentinel), and the
// whole codebase treats Card objects as immutable (the old TS engine aliased
// them freely between hands, battles and logs), so pooled singletons are
// drop-in. Reads (suit, value) come straight off the u8 view.
const HIDDEN_CARD: Card = { suit: -1, value: -1 };
const CARD_POOL: Card[] = [];
for (let s = 0; s < 4; s++) for (let v = 1; v <= 13; v++) CARD_POOL[s * 13 + v - 1] = { suit: s, value: v };
function pooledCard(rawSuit: number, rawValue: number): Card {
    const s = i8(rawSuit), v = i8(rawValue);
    if (s >= 0 && s < 4 && v >= 1 && v <= 13) return CARD_POOL[s * 13 + v - 1];
    if (s === -1 && v === -1) return HIDDEN_CARD;
    return { suit: s, value: v };
}

// 1-byte wire cards (mirrors cnitro/wasm/wire.h): 0..51 = suit*13+(value-1),
// 0xFE the hidden card, 0xFF no card. Encoding reproduces the old 2-byte
// path's semantics exactly: int8 wrap (hostile Infinity/NaN suits wrapped
// through `& 0xff` + i8) then the kernel clamp (suit 0..3, value 1..13).
export const WIRE_HIDDEN = 0xfe;
export const WIRE_NONE = 0xff;
function wireStateCard(c: Card): number {
    let s = i8(c.suit & 0xff), v = i8(c.value & 0xff);
    if (s === -1 && v === -1) return WIRE_HIDDEN;
    if (s < 0) s = 0; else if (s > 3) s = 3;
    if (v < 1) v = 1; else if (v > 13) v = 13;
    return s * 13 + (v - 1);
}
function wireLogCard(c: Card | null | undefined): number {
    if (!c) return WIRE_NONE;
    return wireStateCard(c);
}
function cardFromWire(b: number): Card {
    if (b === WIRE_HIDDEN) return HIDDEN_CARD;
    return CARD_POOL[b <= 51 ? b : 51];
}

// ---------------------------------------------------------------------------
// Marshaling: TS Game <-> kernel byte layout (see wasm_api.c)
// ---------------------------------------------------------------------------

const G_STATUS_TO_INT: Record<string, number> = {
    [GAME_STATUS.WAITING]: 0, [GAME_STATUS.PLAYING]: 1, [GAME_STATUS.GAME_OVER]: 2,
};
const G_STATUS_FROM_INT = [GAME_STATUS.WAITING, GAME_STATUS.PLAYING, GAME_STATUS.GAME_OVER] as const;
const P_STATUS_TO_INT: Record<string, number> = {
    [PLAYER_STATUS.IDLE]: 0, [PLAYER_STATUS.READY]: 1, [PLAYER_STATUS.IN]: 2, [PLAYER_STATUS.OUT]: 3,
};
const P_STATUS_FROM_INT = [PLAYER_STATUS.IDLE, PLAYER_STATUS.READY, PLAYER_STATUS.IN, PLAYER_STATUS.OUT] as const;
const LOG_TYPE_FROM_INT: LogType[] = [
    LOG_TYPE.GAME_START, LOG_TYPE.ATTACK, LOG_TYPE.COVER, LOG_TYPE.PASS,
    LOG_TYPE.PICKUP, LOG_TYPE.GOOD, LOG_TYPE.DISCARD, LOG_TYPE.DEFENDER_CHANGE,
    LOG_TYPE.PLAYER_OUT, LOG_TYPE.DRAW,
];

// Hook tags (mirrors ENGINE_HOOK_* in cnitro/src/game.h).
const HOOK = {
    ATTACK: 1, OUT: 2, COVER: 3, DISCARD: 4, DRAW: 5, DEFENDER_MOVE: 6,
    PASS: 7, PICKUP: 8, MAGIC_TRANSITION: 9, TRASH: 10,
    START_MAGIC: 11, DEAL: 12, FLIPPED: 13, START_DEFENDER: 14,
} as const;

// Reason codes (mirrors ENGINE_REJECT_* in cnitro/src/game.h).
const REJ = {
    NOT_PLAYING: 1, EMPTY: 2, IS_DEFENDER: 3, NOT_DEFENDER: 4, NOT_IN_HAND: 5,
    DUPLICATES: 6, NOT_SAME_VALUE: 7, NOT_FIRST_ATTACKER: 8, VALUE_NOT_ON_TABLE: 9,
    DEFENDER_CAPACITY: 10, NO_UNCOVERED: 11, ATTACK_NOT_ON_TABLE: 12,
    CANNOT_COVER: 13, NO_TABLE_CARDS: 14, COVER_PRESENT: 15, PASS_VALUES: 16,
    PASS_CAPACITY: 17, NOT_IN_STATUS: 18, ALREADY_GOOD: 19, FIRST_MUST_ATTACK: 20,
    PASS_OVERFLOW: 21,
} as const;

const cardDisplay = (card: Card) => `${VALUE_MAP[card.value]} of ${SUIT_MAP[card.suit]}`;
const cardList = (cards: Card[]) => cards.map(cardDisplay).join(', ');
const sameCard = (a: Card, b: Card) => a.suit === b.suit && a.value === b.value;

function seatOf(game: Game, player_id: string): number {
    const idx = game.players.findIndex(p => p.player_id === player_id);
    if (idx < 0) {
        // TS validators checked the playing-state guard before the player
        // lookup; keep that message priority.
        if (game.status !== GAME_STATUS.PLAYING) {
            throw new Error(`Game ${game.id} is not in playing state`);
        }
        throw new Error(`Player ${player_id} not found in game`);
    }
    return idx;
}

// Reconstruct the good_players ARRAY (insertion-ordered) from a kernel mask:
// the pre-action order survives, and the only possible addition is the actor.
function goodPlayersFromMask(mask: number, game: Game, preGood: string[], actorId: string | null): string[] {
    if (mask === 0) return [];
    const inMask = (pid: string) => {
        const s = game.players.findIndex(p => p.player_id === pid);
        return s >= 0 && (mask & (1 << s)) !== 0;
    };
    const out = preGood.filter(inMask);
    if (actorId && inMask(actorId) && !out.includes(actorId)) out.push(actorId);
    return out;
}

function marshalGame(ex: EngineExports, game: Game): void {
    if (residentFor === game && ex === exportsCache) {
        residentFor = null;
        return;
    }
    residentFor = null;
    const buf = mem(ex);
    let q = ex.wasm_io_ptr();
    buf[q++] = G_STATUS_TO_INT[game.status] ?? 0;
    buf[q++] = game.players.length;
    buf[q++] = game.power_suit & 0xff;
    buf[q++] = game.first_attacker & 0xff;
    buf[q++] = game.defender & 0xff;
    buf[q++] = game.discard_pile_length & 0xff;
    buf[q++] = (game.discard_pile_length >> 8) & 0xff;
    buf[q++] = game.flipped ? 1 : 0;
    buf[q++] = game.flipped ? wireStateCard(game.flipped) : 0;
    let mask = 0;
    for (const pid of game.good_players ?? []) {
        const s = game.players.findIndex(p => p.player_id === pid);
        if (s >= 0) mask |= 1 << s;
    }
    buf[q++] = mask & 0xff;
    buf[q++] = (mask >> 8) & 0xff;
    buf[q++] = (mask >> 16) & 0xff;
    buf[q++] = (mask >> 24) & 0xff;
    buf[q++] = game.good_timestamp !== null && game.good_timestamp !== undefined ? 1 : 0;
    buf[q++] = game.deck.length & 0xff;
    buf[q++] = (game.deck.length >> 8) & 0xff;
    for (const c of game.deck) buf[q++] = wireStateCard(c);
    buf[q++] = game.table_battles.length;
    for (const b of game.table_battles) {
        buf[q++] = wireStateCard(b.attack);
        buf[q++] = b.defense ? wireStateCard(b.defense) : WIRE_NONE;
    }
    for (const p of game.players) {
        buf[q++] = P_STATUS_TO_INT[p.status] ?? 0;
        buf[q++] = p.awaiting_attack ? 1 : 0;
        buf[q++] = p.hand.length;
        for (const c of p.hand) buf[q++] = wireStateCard(c);
    }
    buf[q++] = game.elimination_order.length;
    for (const pid of game.elimination_order) {
        const s = game.players.findIndex(p => p.player_id === pid);
        buf[q++] = s & 0xff;
    }
    ex.wasm_import_state();
    // wasm_import_state drops the deterministic-deck flag (the transient IO
    // format doesn't carry it). Re-assert it for seed-dealt games so the bot
    // path — which marshals a JS Game rather than loading the durable blob —
    // pops the pre-shuffled deck on every mid-game refill instead of drawing a
    // RANDOM card. Without this the game stops being reproducible from its deal
    // seed after the opening (the deck was shuffled deterministically, but the
    // draws scrambled it). game.deterministic_deck is set from the blob on load
    // (deserializeGameState) and at the deal (game_lifecycle).
    if (game.deterministic_deck) ex.wasm_set_deterministic_deck(1);
}

export interface KernelState {
    status: number;
    numPlayers: number;
    powerSuit: number;
    firstAttacker: number;
    defender: number;
    discard: number;
    flipped: Card | null;
    goodMask: number;
    hasGoodTs: boolean;
    deck: Card[];
    battles: Battle[];
    players: { status: number; awaiting: boolean; hand: Card[] }[];
    elimination: number[];
}

function parseState(buf: Uint8Array, q: number): KernelState {
    const status = buf[q++];
    const numPlayers = buf[q++];
    const powerSuit = i8(buf[q++]);
    const firstAttacker = i8(buf[q++]);
    const defender = i8(buf[q++]);
    const discard = buf[q] | (buf[q + 1] << 8); q += 2;
    const hasFlipped = buf[q++] !== 0;
    const flippedWire = buf[q++];
    const goodMask = buf[q] | (buf[q + 1] << 8) | (buf[q + 2] << 16) | (buf[q + 3] << 24); q += 4;
    const hasGoodTs = buf[q++] !== 0;
    const deckN = buf[q] | (buf[q + 1] << 8); q += 2;
    const deck: Card[] = new Array(deckN);
    for (let i = 0; i < deckN; i++) deck[i] = cardFromWire(buf[q++]);
    const nBattles = buf[q++];
    const battles: Battle[] = [];
    for (let i = 0; i < nBattles; i++) {
        const attack = cardFromWire(buf[q++]);
        const dw = buf[q++];
        battles.push({ attack, defense: dw === WIRE_NONE ? null : cardFromWire(dw) });
    }
    const players: KernelState['players'] = [];
    for (let i = 0; i < numPlayers; i++) {
        const pStatus = buf[q++];
        const awaiting = buf[q++] !== 0;
        const handN = buf[q++];
        const hand: Card[] = new Array(handN);
        for (let j = 0; j < handN; j++) hand[j] = cardFromWire(buf[q++]);
        players.push({ status: pStatus, awaiting, hand });
    }
    const elimN = buf[q++];
    const elimination: number[] = [];
    for (let i = 0; i < elimN; i++) elimination.push(i8(buf[q++]));
    return {
        status, numPlayers, powerSuit, firstAttacker, defender, discard,
        flipped: hasFlipped ? cardFromWire(flippedWire) : null,
        goodMask, hasGoodTs, deck, battles, players, elimination,
    };
}

export interface KernelLog {
    type: LogType;
    playerIdx: number;      // -1 = system
    defenderIndex: number;  // -1 = n/a
    pairs: { primary: Card; target: Card | null }[];
}

function parseLogs(buf: Uint8Array, q: number): KernelLog[] {
    const n = buf[q] | (buf[q + 1] << 8); q += 2;
    const logs: KernelLog[] = [];
    for (let i = 0; i < n; i++) {
        const type = LOG_TYPE_FROM_INT[buf[q++]];
        const playerIdx = i8(buf[q++]);
        const defenderIndex = i8(buf[q++]);
        const nPairs = buf[q++];
        const pairs: KernelLog['pairs'] = [];
        for (let j = 0; j < nPairs; j++) {
            const primary = cardFromWire(buf[q++]);
            const tw = buf[q++];
            pairs.push({ primary, target: tw === WIRE_NONE ? null : cardFromWire(tw) });
        }
        logs.push({ type, playerIdx, defenderIndex, pairs });
    }
    return logs;
}

// Rebuild a full TS Game from a kernel state, carrying over the identity /
// meta fields the kernel doesn't model (ids, names, version, logs). Matches
// what the TS handlers' cloneGame snapshots carried, except `logs`, which is
// stripped by personalize_game before anything downstream sees it.
function stateToGame(ks: KernelState, template: Game, preGood: string[], actorId: string | null, goodTs: number | null): Game {
    return {
        id: template.id,
        name: template.name,
        deck_length: template.deck_length,
        discard_pile_length: ks.discard,
        flipped: ks.flipped,
        players: ks.players.map((kp, i) => {
            const tp = template.players[i];
            return {
                player_id: tp.player_id,
                status: P_STATUS_FROM_INT[kp.status],
                name: tp.name,
                hand_length: kp.hand.length,
                is_ai: tp.is_ai,
                hand: kp.hand,
                awaiting_attack: kp.awaiting,
                strategy_key: tp.strategy_key,
            } as PrivatePlayer;
        }),
        status: G_STATUS_FROM_INT[ks.status],
        power_suit: ks.powerSuit,
        first_attacker: ks.firstAttacker,
        defender: ks.defender,
        table_battles: ks.battles,
        elimination_order: ks.elimination.map(s => template.players[s].player_id),
        good_timestamp: ks.hasGoodTs ? goodTs : null,
        good_players: goodPlayersFromMask(ks.goodMask, template, preGood, actorId),
        deck: ks.deck,
        logs: [],
        version: template.version,
    };
}

// ---------------------------------------------------------------------------
// Durable state codec (persisted as games.state bytea).
//
// serializeGameState packs a Game into the kernel's VERSIONED persist blob
// (put_state + a leading format-version byte). The blob carries only the
// volatile state (positions, deck, battles, per-seat hands/status, good-mask,
// elimination) — NOT seat identity (player_id/name/strategy_key/is_ai) nor the
// two presentation fields the kernel doesn't model (good_players insertion
// ORDER, good_timestamp wall-clock VALUE). Those are stable/tiny and live in
// the row's roster columns, reattached on the way out via the same
// stateToGame the action path already uses. This is exactly the
// KernelState-vs-template split parseState/stateToGame were built around.
// ---------------------------------------------------------------------------

// The identity + presentation fields the blob omits; supplied from the row's
// roster columns when decoding.
export interface RosterTemplate {
    id: string;
    name: string;
    version?: number;
    deck_length: number;
    players: { player_id: string; name: string; is_ai: boolean; strategy_key: string }[];
    good_players: string[];
    good_timestamp: number | null;
}

// Format version this build's kernel reads/writes (asserts the loaded embed
// matches what the TS side expects; surfaced for callers/tests).
export function stateFormatVersion(): number { return engine().wasm_state_format_version(); }

// Game -> versioned blob. The bytes are copied out of wasm linear memory, so
// they stay valid across later kernel calls / memory growth.
export function serializeGameState(game: Game): Uint8Array {
    const ex = engine();
    marshalGame(ex, game);              // load `game` into the kernel's working state
    const len = ex.wasm_state_serialize(); // write [version | put_state] into g_io
    const base = ex.wasm_io_ptr();
    return mem(ex).slice(base, base + len);
}

// Versioned blob + roster columns -> full Game. Throws (never silently yields
// an empty game) if the blob's version byte is one this kernel can't read.
export function deserializeGameState(bytes: Uint8Array, roster: RosterTemplate): Game {
    const ex = engine();
    const base = ex.wasm_io_ptr();
    mem(ex).set(bytes, base);
    if (!ex.wasm_state_deserialize(bytes.length)) {
        throw new Error(
            `Unreadable game state blob for ${roster.id}: format version ` +
            `${bytes[0]}, kernel reads ${ex.wasm_state_format_version()}`);
    }
    // g_game now holds the loaded state, but no TS Game object owns it yet.
    residentFor = null;
    ex.wasm_export_state();
    const ks = parseState(mem(ex), base);
    // Carry the durable blob's deterministic-deck flag (byte 1, after the
    // version) onto the JS Game so a later marshalGame can re-assert it — the
    // transient import path would otherwise drop it and randomize mid-game
    // draws on the bot loop. See marshalGame / wasm_set_deterministic_deck.
    const deterministicDeck = bytes.length > 1 && bytes[1] !== 0;
    const template = {
        id: roster.id,
        name: roster.name,
        version: roster.version,
        deck_length: roster.deck_length,
        players: roster.players,
    } as unknown as Game;
    // actorId=null: a fresh load adds no new good-player; preGood carries the
    // stored insertion order so goodPlayersFromMask reproduces it exactly.
    const game = stateToGame(ks, template, roster.good_players, null, roster.good_timestamp);
    // deck lives in the blob, so deck_length is authoritative from it (the
    // roster's is only a placeholder for stateToGame's template slot).
    game.deck_length = game.deck.length;
    game.deterministic_deck = deterministicDeck;
    return game;
}

// Apply a kernel state onto the live Game object in place (the handlers
// mutate their argument, and callers hold references to it).
function applyStateToGame(game: Game, ks: KernelState, actorId: string | null): void {
    const preGoodTs = game.good_timestamp;
    game.discard_pile_length = ks.discard;
    game.flipped = ks.flipped;
    game.status = G_STATUS_FROM_INT[ks.status];
    game.power_suit = ks.powerSuit;
    game.first_attacker = ks.firstAttacker;
    game.defender = ks.defender;
    game.table_battles = ks.battles;
    game.deck = ks.deck;
    for (let i = 0; i < game.players.length; i++) {
        const p = game.players[i];
        p.status = P_STATUS_FROM_INT[ks.players[i].status];
        p.awaiting_attack = ks.players[i].awaiting;
        p.hand = ks.players[i].hand;
        p.hand_length = ks.players[i].hand.length;
    }
    game.elimination_order = ks.elimination.map(s => game.players[s].player_id);
    // good_players: the kernel tracks a set; the TS array order is insertion
    // order, and the only inserter is the good action itself.
    if (ks.goodMask === 0) {
        game.good_players = [];
    } else if (actorId && !game.good_players.includes(actorId)) {
        game.good_players = [...game.good_players, actorId];
    }
    // good_timestamp: the kernel tracks presence; the wall-clock value is
    // presentation (set when a cover completes the table, kept otherwise).
    game.good_timestamp = ks.hasGoodTs ? (preGoodTs ?? Date.now()) : null;
}

// ---------------------------------------------------------------------------
// Log translation
// ---------------------------------------------------------------------------

// DRAW logs hide card identities ({suit:-1,value:-1}) except the flipped
// trump card, whose draw is public — exactly the TS refill convention.
export function appendLogs(game: Game, kernelLogs: KernelLog[], preFlipped: Card | null, postFlipped: Card | null): void {
    const flippedWasDrawn = preFlipped !== null && postFlipped === null;
    for (const kl of kernelLogs) {
        let pairs = kl.pairs.map(p => ({ primary: p.primary, target: p.target }));
        if (kl.type === LOG_TYPE.DRAW) {
            pairs = kl.pairs.map(p => ({
                primary: flippedWasDrawn && sameCard(p.primary, preFlipped!)
                    ? p.primary
                    : HIDDEN_CARD,
                target: null,
            }));
        }
        game.logs.push({
            id: crypto.randomUUID(),
            created_at: new Date().toISOString(),
            game_id: game.id,
            log_type: kl.type,
            player_id: kl.playerIdx >= 0 ? game.players[kl.playerIdx].player_id : null,
            card_pairs: pairs,
            defender_index: kl.defenderIndex >= 0 ? kl.defenderIndex : null,
        } as GameLog);
    }
}

// ---------------------------------------------------------------------------
// Packed wire pipeline (docs/PACKED_WIRE_CUTOVER.md)
//
// The human-move hot path: state blob in, action wire applied, state blob +
// per-viewer masked event streams out — no TS Game object anywhere. All
// kernel work happens synchronously inside one call (the resident kernel
// state is a module singleton; an `await` between load and export would let
// a concurrently-handled request clobber it).
// ---------------------------------------------------------------------------

export interface PackedRunReject { ok: false; reason: number }
export interface PackedRunOk {
    ok: true;
    fool: number;                     // fool seat, -1 while the game runs
    ended: boolean;
    stateBlob: Uint8Array;            // versioned durable blob, post-finalize
    post: KernelState;                // for the JSONB public dual / roster columns
    logsWire: Uint8Array;             // this action's kernel log export, DRAW-masked
    nEvents: number;
    events: Map<number, Uint8Array>;  // viewer seat (-1 = spectator) -> evwire bytes
}

export function runPackedAction(
    blob: Uint8Array, seat: number, wire: Uint8Array,
    aiMask: number, humanSeats: number[],
): PackedRunOk | PackedRunReject {
    const ex = engine();
    mem(ex).set(blob, ex.wasm_io_ptr());
    if (!ex.wasm_state_deserialize(blob.length)) {
        throw new Error(`Unreadable game state blob: format version ${blob[0]}, kernel reads ${ex.wasm_state_format_version()}`);
    }
    residentFor = null;
    return packedActionCore(ex, seat, wire, aiMask, humanSeats);
}

// Same pipeline from a JS Game instead of a blob — the bot loop's entry: a
// kernel-brained bot that just chose a move left the resident state valid
// (wasmChooseMove's marshal-skip contract), so this marshal is usually free;
// a cached-replay or gpt move pays one normal marshal. Everything after the
// import is identical to the blob path.
export function runPackedGameAction(
    game: Game, seat: number, wire: Uint8Array,
    aiMask: number, humanSeats: number[],
): PackedRunOk | PackedRunReject {
    const ex = engine();
    marshalGame(ex, game);
    return packedActionCore(ex, seat, wire, aiMask, humanSeats);
}

function packedActionCore(
    ex: EngineExports, seat: number, wire: Uint8Array,
    aiMask: number, humanSeats: number[],
): PackedRunOk | PackedRunReject {
    const buf = mem(ex);

    // Mid-game LCG seed. Tests pin it via seedSource so the parity suite stays
    // byte-for-byte. Live, seed it DETERMINISTICALLY from the current game state
    // (itself a pure function of the deal seed) instead of Math.random — so the
    // whole game replays from its deal seed. Crypto is drawn exactly once, at
    // the deal (injectDealSeed); no per-move randomness after that.
    if (seedSource) ex.wasm_set_seed(seedSource() >>> 0);
    else ex.wasm_seed_rng_deterministic();

    if (wire.length > 128) throw new Error('malformed action wire');
    buf.set(wire, ex.wasm_cards_a_ptr());
    const r = ex.wasm_apply_action(seat, wire.length);
    if (r < 0) throw new Error('malformed action wire');
    if (r === 0) return { ok: false, reason: ex.wasm_reject_reason() };

    const fool = ex.wasm_finalize_win(aiMask >>> 0);
    return exportPackedProducts(ex, seat, fool, humanSeats);
}

// The export tail every packed mutation shares: durable blob + public-dual
// state + masked log records + per-recipient event streams, all read out of
// the resident kernel synchronously.
function exportPackedProducts(
    ex: EngineExports, actorSeat: number, fool: number, humanSeats: number[],
): PackedRunOk {
    const base = ex.wasm_io_ptr();
    const ended = fool >= 0;

    const blobLen = ex.wasm_state_serialize();
    const stateBlob = mem(ex).slice(base, base + blobLen);
    ex.wasm_export_state();
    const post = parseState(mem(ex), base);
    // The DRAW-privacy masking happens inside the kernel (it captured the
    // pre-action flip in begin_action); these bytes go straight to the
    // packed session-log column — no JS log objects on this path.
    const logsLen = ex.wasm_export_logs_masked();
    const logsWire = mem(ex).slice(base, base + logsLen);

    // Per-recipient event streams — serialized before anything else touches
    // the kernel (the snapshots live in bridge statics).
    const events = new Map<number, Uint8Array>();
    let nEvents = 0;
    for (const viewer of [...humanSeats, -1]) {
        const len = ex.wasm_events_serialize(viewer, actorSeat, ended ? 1 : 0);
        if (len < 0) throw new Error('event stream serialization overflow');
        const bytes = mem(ex).slice(base, base + len);
        nEvents = bytes[3];
        events.set(viewer, bytes);
    }
    return { ok: true, fool, ended, stateBlob, post, logsWire, nEvents, events };
}

// The game start (deal/flip/first-attacker) as a packed mutation — the
// fattest broadcast in the game goes kernel-native. A start can't end the
// game and has no acting seat (system event).
export function runPackedStart(game: Game, humanSeats: number[]): PackedRunOk {
    const ex = engine();
    marshalGame(ex, game);
    injectDealSeed(ex);   // live: full-universe ChaCha deal; test: pinned LCG
    ex.wasm_start_game();
    return exportPackedProducts(ex, -1, -1, humanSeats);
}

// Kernel-validated hand rearrange (the permutation check that prevents
// duplicate-card minting lives in wasm_rearrange_hand). Emits no events and
// no logs — only the reordered durable blob. Returns null on an invalid
// permutation (resident state untouched).
export function runPackedRearrange(
    game: Game, seat: number, indices: number[],
): { stateBlob: Uint8Array; post: KernelState } | null {
    const ex = engine();
    marshalGame(ex, game);
    if (indices.length > 128) return null;
    const buf = mem(ex);
    const ptr = ex.wasm_cards_a_ptr();
    for (let i = 0; i < indices.length; i++) buf[ptr + i] = indices[i] & 0xff;
    if (!ex.wasm_rearrange_hand(seat, indices.length)) return null;
    const base = ex.wasm_io_ptr();
    const blobLen = ex.wasm_state_serialize();
    const stateBlob = mem(ex).slice(base, base + blobLen);
    ex.wasm_export_state();
    return { stateBlob, post: parseState(mem(ex), base) };
}

// Materialize a full TS Game from a kernel state + roster columns — the cold
// paths that still want the JS object (the commit's JSONB public dual, the
// end-of-game ELO/replay finalize). Unlike deserializeGameState this takes
// the acting player so the good_players insertion-order rule matches
// applyStateToGame exactly.
export function materializeKernelGame(post: KernelState, roster: RosterTemplate, actorId: string | null): Game {
    const template = {
        id: roster.id,
        name: roster.name,
        version: roster.version,
        deck_length: roster.deck_length,
        players: roster.players,
    } as unknown as Game;
    const game = stateToGame(post, template, roster.good_players, actorId, roster.good_timestamp ?? Date.now());
    game.deck_length = game.deck.length;
    return game;
}

// In-place kernel-state apply for callers that hold references to the Game
// (the bot loop mutates one shared object across its cycle) — the packed
// counterpart of what execute() does after a JSON-path action.
export function applyKernelStateToGame(game: Game, post: KernelState, actorId: string | null): void {
    applyStateToGame(game, post, actorId);
}

// Per-viewer masked view blob from a durable state blob — get_game's packed
// response body. Synchronous single kernel section, same discipline as
// runPackedAction.
export function serializeViewBlob(blob: Uint8Array, viewerSeat: number): Uint8Array {
    const ex = engine();
    const base = ex.wasm_io_ptr();
    mem(ex).set(blob, base);
    if (!ex.wasm_state_deserialize(blob.length)) {
        throw new Error(`Unreadable game state blob: format version ${blob[0]}, kernel reads ${ex.wasm_state_format_version()}`);
    }
    residentFor = null;
    const len = ex.wasm_view_serialize(viewerSeat);
    return mem(ex).slice(base, base + len);
}

// The per-VIEWER masked blobs for MANY seats from one durable state blob — the
// player_views cache writer (one row per participant per commit). The masking
// itself stays entirely in the C kernel (wasm_view_serialize / view.c
// state_put); this only deserializes ONCE and reads the resident g_game out per
// seat, so N views cost one deserialize + N put_state instead of N full
// re-deserializes. wasm_view_serialize is read-only on g_game (it writes into
// g_io, the game is untouched), so the loop is safe — same one-load-many-export
// discipline as exportPackedProducts' event loop. Returns viewerSeat -> blob.
export function serializeViewBlobs(blob: Uint8Array, viewerSeats: number[]): Map<number, Uint8Array> {
    const ex = engine();
    const base = ex.wasm_io_ptr();
    mem(ex).set(blob, base);
    if (!ex.wasm_state_deserialize(blob.length)) {
        throw new Error(`Unreadable game state blob: format version ${blob[0]}, kernel reads ${ex.wasm_state_format_version()}`);
    }
    residentFor = null;
    const out = new Map<number, Uint8Array>();
    for (const seat of viewerSeats) {
        const len = ex.wasm_view_serialize(seat);
        out.set(seat, mem(ex).slice(base, base + len));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Action execution + event synthesis
// ---------------------------------------------------------------------------

interface Snapshot { tag: number; aux: number; state: KernelState }

interface KernelRun {
    ok: boolean;
    reason: number;
    post: KernelState | null;
    logs: KernelLog[];
    snaps: Snapshot[];
}

function writeCards(ex: EngineExports, ptr: number, cards: Card[]): void {
    const buf = mem(ex);
    for (let i = 0; i < cards.length; i++) buf[ptr + i] = wireStateCard(cards[i]);
}

type KernelAction =
    | { kind: 'attack'; seat: number; cards: Card[] }
    | { kind: 'cover'; seat: number; cards: Card[]; attacks: Card[] }
    | { kind: 'pass'; seat: number; cards: Card[] }
    | { kind: 'pickup'; seat: number }
    | { kind: 'good'; seat: number }
    | { kind: 'start' }
    | { kind: 'transition' }
    | { kind: 'refill' };

// Test hook: differential harnesses inject a deterministic seed source so
// kernel draws replay the exact sequence a seeded Math.random produced.
let seedSource: (() => number) | null = null;
export function __setKernelSeedSource(fn: (() => number) | null): void { seedSource = fn; }

// Test hook: pin the 32-byte deal seed so a deal reproduces a KNOWN game (used
// by the determinism suite to replay one deal across processes). Null clears it
// back to the live crypto deal. Ignored under seedSource (that path stays LCG).
let dealSeedOverride: Uint8Array | null = null;
export function __setDealSeedOverride(seed: Uint8Array | null): void { dealSeedOverride = seed; }

// 32-bit base for the mid-game bot RNG, derived from the SERVER-ONLY deal seed
// (game.game_seed, never sent to a client). Seeding the bots from this instead
// of the public board is a security boundary: a player — even one with the
// source — can't recompute the seed without game_seed, so they can't predict
// octogen's world sampling. FNV-1a of the 64-hex-char seed; 0 when there is no
// deal seed (tests use seedSource to pin the stream, legacy games have none).
// One-way, so the base leaking (it never does) still wouldn't reveal game_seed.
export function rngBaseFromSeed(hex?: string | null): number {
    if (!hex) return 0;
    let h = 2166136261;
    for (let i = 0; i < hex.length; i++) h = Math.imul(h ^ hex.charCodeAt(i), 16777619);
    return h >>> 0;
}

// The 32-byte (two-128-bit-lane) seed of the most recent LIVE deal, or null if
// the last deal used the deterministic test path. Persist this to reproduce a
// deal exactly (see cnitro/src/deal_rng.h). Overwritten on each live deal.
let lastDealSeed: Uint8Array | null = null;
export function getLastDealSeed(): Uint8Array | null { return lastDealSeed; }

// The most recent live deal's seed as 64 lowercase hex chars, or null if the
// last deal used the deterministic test path. Persist this (games.game_seed) to
// regenerate the deal for audit/replay.
export function getLastDealSeedHex(): string | null {
    if (!lastDealSeed) return null;
    let s = '';
    for (const b of lastDealSeed) s += b.toString(16).padStart(2, '0');
    return s;
}

// Seed the DEAL. Under a test seedSource we keep the pinned 32-bit LCG (deals
// stay byte-for-byte reproducible for the parity suite). Live, we draw 32
// crypto bytes and hand them to the kernel's ChaCha deal — lifting reachable
// deals from 2^32 to the whole 52!/36! space and making the deal reproducible
// from the saved bytes. Call AFTER marshalGame (it consumes the io buffer via
// wasm_import_state, so the front of the buffer is free to carry the seed).
function injectDealSeed(ex: EngineExports): void {
    if (seedSource) { ex.wasm_set_seed(seedSource() >>> 0); lastDealSeed = null; return; }
    const seed = dealSeedOverride ? dealSeedOverride.slice() : new Uint8Array(32);
    if (!dealSeedOverride) crypto.getRandomValues(seed);
    mem(ex).set(seed, ex.wasm_io_ptr());
    ex.wasm_set_deal_seed_bytes();
    lastDealSeed = seed;
}

function runKernel(game: Game, action: KernelAction): KernelRun {
    const ex = engine();
    marshalGame(ex, game);
    // Seed kernel randomness. The initial deal ('start') gets the wide,
    // full-universe crypto seed (injectDealSeed) — the ONLY crypto draw in a
    // game. Every mid-game move seeds the LCG DETERMINISTICALLY from the current
    // state (tests still pin it via seedSource), so the game replays exactly
    // from its deal seed instead of the old per-move Math.random reseed.
    if (action.kind === 'start') injectDealSeed(ex);
    else if (seedSource) ex.wasm_set_seed(seedSource() >>> 0);
    else { ex.wasm_set_rng_base(rngBaseFromSeed(game.game_seed)); ex.wasm_seed_rng_deterministic(); }

    let ok = 1;
    switch (action.kind) {
        case 'attack':
            writeCards(ex, ex.wasm_cards_a_ptr(), action.cards);
            ok = ex.wasm_attack(action.seat, action.cards.length);
            break;
        case 'cover':
            writeCards(ex, ex.wasm_cards_a_ptr(), action.cards);
            writeCards(ex, ex.wasm_cards_b_ptr(), action.attacks);
            ok = ex.wasm_cover(action.seat, action.cards.length);
            break;
        case 'pass':
            writeCards(ex, ex.wasm_cards_a_ptr(), action.cards);
            ok = ex.wasm_pass(action.seat, action.cards.length);
            break;
        case 'pickup': ok = ex.wasm_pickup(action.seat); break;
        case 'good': ok = ex.wasm_good(action.seat); break;
        case 'start': ok = ex.wasm_start_game(); break;
        case 'transition': ok = ex.wasm_transition(); break;
        case 'refill': ok = ex.wasm_refill(); break;
    }

    if (!ok) return { ok: false, reason: ex.wasm_reject_reason(), post: null, logs: [], snaps: [] };

    const base = ex.wasm_io_ptr();
    ex.wasm_export_state();
    const post = parseState(mem(ex), base);
    ex.wasm_export_logs();
    const logs = parseLogs(mem(ex), base);
    const snaps: Snapshot[] = [];
    const n = ex.wasm_snap_count();
    for (let i = 0; i < n; i++) {
        ex.wasm_export_snapshot(i);
        snaps.push({
            tag: ex.wasm_snap_tag(i),
            aux: ex.wasm_snap_aux(i),
            state: parseState(mem(ex), base),
        });
    }
    return { ok: true, reason: 0, post, logs, snaps };
}

// Synthesize the production AnimationEvent stream from the kernel's hook
// snapshots + logs. Every event's game_state is the full intermediate Game,
// exactly as the TS handlers captured with cloneGame.
function buildEvents(game: Game, run: KernelRun, ctx: { reason?: string; actorId?: string | null }): AnimationEvent[] {
    const events: AnimationEvent[] = [];
    const name = (seat: number) => game.players[seat].name;
    const pid = (seat: number) => game.players[seat].player_id;

    // The good_players/good_timestamp of intermediate snapshots: reconstruct
    // per snapshot from its own mask, with the same insertion-order rule as
    // the live object.
    const preGood = game.good_players;
    const preGoodTs = game.good_timestamp;
    const snapGame = (s: Snapshot) =>
        stateToGame(s.state, game, preGood, ctx.actorId ?? null, preGoodTs ?? Date.now());

    // Sequential readers for per-type logs (each DRAW/COVER hook consumes the
    // next matching log).
    const drawLogs = run.logs.filter(l => l.type === LOG_TYPE.DRAW);
    const coverLogs = run.logs.filter(l => l.type === LOG_TYPE.COVER);
    const discardLog = run.logs.find(l => l.type === LOG_TYPE.DISCARD);
    let drawI = 0, coverI = 0;

    for (const s of run.snaps) {
        switch (s.tag) {
            case HOOK.ATTACK: {
                const log = run.logs.find(l => l.type === LOG_TYPE.ATTACK);
                const cards = log ? log.pairs.map(p => p.primary) : [];
                events.push({
                    type: ANIMATION_EVENT_TYPE.ATTACK_PASS,
                    player_id: pid(s.aux),
                    cards,
                    from_location: 'hand',
                    to_location: 'table',
                    message: `${name(s.aux)} attacked with ${cardList(cards)}`,
                    game_state: snapGame(s),
                });
                break;
            }
            case HOOK.PASS: {
                const log = run.logs.find(l => l.type === LOG_TYPE.PASS);
                const cards = log ? log.pairs.map(p => p.primary) : [];
                events.push({
                    type: ANIMATION_EVENT_TYPE.ATTACK_PASS,
                    player_id: pid(s.aux),
                    cards,
                    from_location: 'hand',
                    to_location: 'table',
                    message: `${name(s.aux)} passed with ${cardList(cards)}`,
                    game_state: snapGame(s),
                });
                break;
            }
            case HOOK.OUT:
                events.push({
                    type: ANIMATION_EVENT_TYPE.OUT,
                    player_id: pid(s.aux),
                    message: `${name(s.aux)} is out`,
                    game_state: snapGame(s),
                });
                break;
            case HOOK.COVER: {
                const log = coverLogs[coverI++];
                const cover = log.pairs[0].primary;
                const attack = log.pairs[0].target!;
                const defenderSeat = seatCoveredBy(run, s);
                events.push({
                    type: ANIMATION_EVENT_TYPE.COVER,
                    player_id: pid(defenderSeat),
                    cards: [cover],
                    target_card: attack,
                    battle_index: s.aux,
                    from_location: 'hand',
                    to_location: 'table',
                    message: `${name(defenderSeat)} covered ${cardDisplay(attack)} with ${cardDisplay(cover)}`,
                    game_state: snapGame(s),
                });
                break;
            }
            case HOOK.DISCARD: {
                const cards = discardLog ? discardLog.pairs.map(p => p.primary) : [];
                events.push({
                    type: ANIMATION_EVENT_TYPE.DISCARD,
                    cards,
                    from_location: 'table',
                    to_location: 'discard',
                    message: `${cards.length} cards discarded`,
                    game_state: snapGame(s),
                });
                break;
            }
            case HOOK.TRASH: {
                const cards = discardLog ? discardLog.pairs.map(p => p.primary) : [];
                if (cards.length > 0) {
                    events.push({
                        type: ANIMATION_EVENT_TYPE.CARDS_TO_TRASH,
                        cards,
                        from_location: 'table',
                        to_location: 'discard',
                        message: `${cards.length} cards discarded`,
                        game_state: snapGame(s),
                    });
                }
                break;
            }
            case HOOK.DRAW: {
                const log = drawLogs[drawI++];
                const cards = log.pairs.map(p => p.primary); // real identities for the recipient
                events.push({
                    type: ANIMATION_EVENT_TYPE.REFILL,
                    player_id: pid(s.aux),
                    cards,
                    from_location: 'deck',
                    to_location: 'hand',
                    message: `${name(s.aux)} drew ${cards.length} cards`,
                    game_state: snapGame(s),
                });
                break;
            }
            case HOOK.DEFENDER_MOVE:
                events.push({
                    type: ANIMATION_EVENT_TYPE.DEFENDER_MOVE,
                    player_id: pid(s.aux),
                    message: `${name(s.aux)} is now the defender`,
                    game_state: snapGame(s),
                });
                break;
            case HOOK.PICKUP: {
                const log = run.logs.find(l => l.type === LOG_TYPE.PICKUP);
                const cards = log ? log.pairs.map(p => p.primary) : [];
                events.push({
                    type: ANIMATION_EVENT_TYPE.PICKUP,
                    player_id: pid(s.aux),
                    cards,
                    from_location: 'table',
                    to_location: 'hand',
                    message: `${name(s.aux)} picked up ${cards.length} cards`,
                    game_state: snapGame(s),
                });
                break;
            }
            case HOOK.MAGIC_TRANSITION: {
                const reason = ctx.reason ?? transitionReason(s.state, game);
                events.push({
                    type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
                    message: `${reason} - proceeding to next round`,
                    game_state: snapGame(s),
                });
                break;
            }
            case HOOK.START_MAGIC:
                events.push({
                    type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
                    message: `All players ready - starting game!`,
                    game_state: snapGame(s),
                });
                break;
            case HOOK.DEAL:
                events.push({
                    type: ANIMATION_EVENT_TYPE.DEAL,
                    player_id: pid(s.aux),
                    cards: s.state.players[s.aux].hand,
                    from_location: 'deck',
                    to_location: 'hand',
                    game_state: snapGame(s),
                });
                break;
            case HOOK.FLIPPED:
                events.push({
                    type: ANIMATION_EVENT_TYPE.FLIPPED,
                    cards: [s.state.flipped!],
                    from_location: 'deck',
                    to_location: 'flipped',
                    game_state: snapGame(s),
                });
                break;
            case HOOK.START_DEFENDER: {
                events.push({
                    type: ANIMATION_EVENT_TYPE.DEFENDER_MOVE,
                    player_id: pid(s.aux),
                    game_state: snapGame(s),
                });
                events.push({
                    type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
                    message: `Player ${name(s.state.firstAttacker)} is the first attacker, wait for them to attack`,
                    game_state: snapGame(s),
                });
                break;
            }
        }
    }
    return events;
}

// The seat that performed a cover is the defender at snapshot time.
function seatCoveredBy(_run: KernelRun, s: Snapshot): number {
    return s.state.defender;
}

// TS good.ts computed the transition reason from the attacker count.
function transitionReason(preDiscard: KernelState, game: Game): string {
    let attackers = 0;
    for (let i = 0; i < game.players.length; i++) {
        if (i !== preDiscard.defender && preDiscard.players[i].status === P_STATUS_TO_INT[PLAYER_STATUS.IN]) {
            attackers++;
        }
    }
    return `All ${attackers} attackers said good and all attacks covered`;
}

// ---------------------------------------------------------------------------
// Rejection reasons -> the production error messages
// ---------------------------------------------------------------------------

function rejectionError(
    game: Game, reason: number, player_id: string,
    cards: Card[], attacks: Card[] | null, kind: KernelAction['kind'],
): Error {
    const inHand = (p: PrivatePlayer, c: Card) => p.hand.some(h => sameCard(h, c));
    const player = game.players.find(p => p.player_id === player_id);
    switch (reason) {
        case REJ.NOT_PLAYING: return new Error(`Game ${game.id} is not in playing state`);
        case REJ.EMPTY: return new Error('No cards provided');
        case REJ.IS_DEFENDER:
            // good has its own wording; attack goes through validate_defender_status.
            if (kind === 'good') return new Error('Defender cannot say good');
            return new Error(`Player ${player_id} is the defender`);
        case REJ.NOT_DEFENDER: return new Error(`Player ${player_id} is not the defender`);
        case REJ.NOT_IN_HAND: {
            const missing = player ? cards.find(c => !inHand(player, c)) : cards[0];
            return new Error(`Card ${cardDisplay(missing ?? cards[0])} is not in player ${player_id}'s hand`);
        }
        case REJ.DUPLICATES: {
            const dup = (arr: Card[]) => new Set(arr.map(c => `${c.suit}-${c.value}`)).size !== arr.length;
            const list = dup(cards) || !attacks ? cards : attacks;
            return new Error(`Cards ${cardList(list)} have duplicates`);
        }
        case REJ.NOT_SAME_VALUE: return new Error(`Cards ${cardList(cards)} are not all the same value`);
        case REJ.NOT_FIRST_ATTACKER: return new Error(`Player ${player_id} is not the first attacker`);
        case REJ.VALUE_NOT_ON_TABLE: return new Error(`Some card values of ${cardList(cards)} are not on the table`);
        case REJ.DEFENDER_CAPACITY: {
            const defender = game.players[game.defender];
            const uncovered = game.table_battles.filter(b => b.defense === null).length;
            return new Error(`Defender ${defender.name} only has ${defender.hand.length} card(s) but would need to cover ${uncovered + cards.length} attacks`);
        }
        case REJ.NO_UNCOVERED: return new Error('No uncovered attacks to cover');
        case REJ.ATTACK_NOT_ON_TABLE: {
            const onTable = (c: Card) => game.table_battles.some(b => sameCard(b.attack, c) && b.defense === null);
            const missing = (attacks ?? []).find(c => !onTable(c)) ?? (attacks ?? [])[0];
            return new Error(`Card ${cardDisplay(missing)} is not on the table`);
        }
        case REJ.CANNOT_COVER: {
            for (let i = 0; i < cards.length; i++) {
                const a = attacks![i], c = cards[i];
                const covers = c.suit === a.suit ? c.value > a.value
                    : c.suit === game.power_suit && a.suit !== game.power_suit;
                if (!covers) return new Error(`Card ${cardDisplay(c)} cannot cover ${cardDisplay(a)}`);
            }
            return new Error(`Card ${cardDisplay(cards[0])} cannot cover ${cardDisplay(attacks![0])}`);
        }
        case REJ.NO_TABLE_CARDS: return new Error('No cards on the table');
        case REJ.COVER_PRESENT: return new Error('Cover present, cannot pass');
        case REJ.PASS_VALUES: return new Error(`Cards ${cardList(cards)} do not match the values on the table`);
        case REJ.PASS_CAPACITY: {
            const ex = engine();
            const next = game.players[ex.wasm_next_player(game.defender)];
            return new Error(`Player ${next.name} does not have enough cards in their hand to cover ${cardList(cards)}`);
        }
        case REJ.NOT_IN_STATUS: return new Error(`Player ${player_id} is not ready to attack`);
        case REJ.ALREADY_GOOD: return new Error('Player has already said good');
        case REJ.FIRST_MUST_ATTACK: return new Error('First attacker must attack - cannot say good with empty table');
        case REJ.PASS_OVERFLOW: return new Error('Uncovered cards > defender_cards');
        default: return new Error(`Move rejected (reason ${reason})`);
    }
}

// ---------------------------------------------------------------------------
// Public API — the rules surface actions/*.ts and common_utils.ts delegate to
// ---------------------------------------------------------------------------

function execute(game: Game, action: KernelAction, actorId: string | null, ctx: { reason?: string } = {}): AnimationEvent[] {
    // start_game / executeRoundTransition mirror the TS no-op guard on a
    // finished game; the five player actions reject via the kernel instead
    // (handleX must throw "not in playing state", exactly like validateX).
    if ((action.kind === 'start' || action.kind === 'transition' || action.kind === 'refill')
        && game.status === GAME_STATUS.GAME_OVER) return [];
    const preFlipped = game.flipped;
    const run = runKernel(game, action);
    if (!run.ok) {
        const cards = 'cards' in action ? action.cards : [];
        const attacks = action.kind === 'cover' ? action.attacks : null;
        throw rejectionError(game, run.reason, actorId ?? '', cards, attacks, action.kind);
    }
    const events = buildEvents(game, run, { ...ctx, actorId });
    applyStateToGame(game, run.post!, actorId);
    appendLogs(game, run.logs, preFlipped, game.flipped);
    return events;
}

// Run the action against the kernel purely for the accept/reject verdict —
// the validateX surface of the old TS handlers. The kernel state is
// re-marshaled per call, so the live Game is never touched.
export function kernelValidateAttack(game: Game, player_id: string, cards: Card[]): void {
    validateOnly(game, { kind: 'attack', seat: seatOf(game, player_id), cards }, player_id);
}
export function kernelValidateCover(game: Game, player_id: string, cover_cards: Card[], attack_cards: Card[]): void {
    validateOnly(game, { kind: 'cover', seat: seatOf(game, player_id), cards: cover_cards, attacks: attack_cards }, player_id);
}
export function kernelValidatePass(game: Game, player_id: string, cards: Card[]): void {
    validateOnly(game, { kind: 'pass', seat: seatOf(game, player_id), cards }, player_id);
}
export function kernelValidatePickup(game: Game, player_id: string): void {
    validateOnly(game, { kind: 'pickup', seat: seatOf(game, player_id) }, player_id);
}
export function kernelValidateGood(game: Game, player_id: string): void {
    validateOnly(game, { kind: 'good', seat: seatOf(game, player_id) }, player_id);
}

function validateOnly(game: Game, action: KernelAction, actorId: string): void {
    const run = runKernel(game, action);
    if (!run.ok) {
        const cards = 'cards' in action ? action.cards : [];
        const attacks = action.kind === 'cover' ? action.attacks : null;
        throw rejectionError(game, run.reason, actorId, cards, attacks, action.kind);
    }
}

export function kernelAttack(game: Game, player_id: string, cards: Card[]): AnimationEvent[] {
    return execute(game, { kind: 'attack', seat: seatOf(game, player_id), cards }, player_id);
}

export function kernelCover(game: Game, player_id: string, cover_cards: Card[], attack_cards: Card[]): AnimationEvent[] {
    return execute(game, { kind: 'cover', seat: seatOf(game, player_id), cards: cover_cards, attacks: attack_cards }, player_id);
}

export function kernelPass(game: Game, player_id: string, cards: Card[]): AnimationEvent[] {
    return execute(game, { kind: 'pass', seat: seatOf(game, player_id), cards }, player_id);
}

export function kernelPickup(game: Game, player_id: string): AnimationEvent[] {
    return execute(game, { kind: 'pickup', seat: seatOf(game, player_id) }, player_id);
}

export function kernelGood(game: Game, player_id: string): AnimationEvent[] {
    return execute(game, { kind: 'good', seat: seatOf(game, player_id) }, player_id);
}

export function kernelStartGame(game: Game): AnimationEvent[] {
    return execute(game, { kind: 'start' }, null);
}

export function kernelRoundTransition(game: Game, reason: string): AnimationEvent[] {
    return execute(game, { kind: 'transition' }, null, { reason });
}

// The old refillPlayerHandsWithEvents compatibility wrapper (kernelRefill)
// was deleted with its last caller: the action handlers run refill inside
// their kernel transitions (wasm_refill stays exported for tests/tools).

// Kernel-side counterparts of the thin TS projections that stayed in
// common_utils/pure_bot_actions for the client's synchronous use (canCover,
// game_done, get_next_player_index, shouldBotActCore). Exported so tests can
// police that the TS copies never drift from the kernel.
// These are state READERS: clear the resident mark before marshaling (the
// __setResident invariant — only the action executing a just-chosen move may
// skip the marshal), same as kernelLegalMoves below. Their callers are
// test-only today, but the invariant must hold for every reader.
export function kernelGameDone(game: Game): string | null {
    const ex = engine();
    residentFor = null;
    marshalGame(ex, game);
    const loser = ex.wasm_game_done();
    return loser >= 0 ? game.players[loser].player_id : null;
}

export function kernelShouldAct(game: Game, player_id: string): boolean {
    const seat = game.players.findIndex(p => p.player_id === player_id);
    if (seat < 0) return false;
    const ex = engine();
    residentFor = null;
    marshalGame(ex, game);
    return ex.wasm_should_act(seat) !== 0;
}

export function kernelNextPlayer(game: Game, current: number): number {
    const ex = engine();
    residentFor = null;
    marshalGame(ex, game);
    return ex.wasm_next_player(current);
}

export function kernelCanCover(attack: Card, defense: Card, powerSuit: number): boolean {
    return engine().wasm_can_cover(attack.suit, attack.value, defense.suit, defense.value, powerSuit) !== 0;
}

// ---------------------------------------------------------------------------
// Replay codec (cnitro/src/replay.c) — the format-v5 rules projection runs in
// the kernel; TS only moves bytes. Formats are documented in replay.h. The
// replay buffers are separate from the game IO, so these calls never disturb
// a resident game state.
// ---------------------------------------------------------------------------

// Mirrors REPLAY_E* in replay.h. Messages match the TS reference's throws,
// except the conservation desync (the reference interpolated the live
// counts; the kernel reports only the code) and the C-side-only EINPUT/ECAP.
function replayError(negCode: number, detail: number): Error {
    switch (-negCode) {
        case 1: return new Error(`unsupported replay format version ${detail}`);
        case 2: return new Error('invalid replay: leftover data after game end');
        case 3: return new Error('invalid replay: no single fool');
        case 4: return new Error('replay guard: too many events');
        case 5: return new Error('replay desync: no legal moves');
        case 6: return new Error('replay desync: conservation');
        case 7: return new Error('replay desync: known card');
        case 8: return new Error('replay desync: fresh card');
        case 9: return new Error('replay desync: hidden count');
        case 10: return new Error('replay desync: no fresh card');
        case 11: return new Error('replay desync: fresh card not feasible');
        case 12: return new Error(
            `replay desync: logged ${LOG_TYPE_FROM_INT[detail >> 16] ?? (detail >> 16)} not in menu of ${detail & 0xffff}`);
        case 13: return new Error('replay desync: round end not in menu');
        case 14: return new Error('replay desync: attack continuation');
        case 15: return new Error('replay desync: pass continuation');
        case 16: return new Error('incomplete game: logs ended before the fool was known');
        case 17: return new Error('replay desync: logs continue after the game ended');
        case 18: return new Error('empty menu');
        case 19: return new Error('encode: chosen index out of range');
        case 20: return new Error('trump not in alphabet');
        case 21: return new Error('replay: malformed encode input');
        case 22: return new Error('replay: capacity exceeded');
        default: return new Error(`replay kernel error ${negCode}`);
    }
}

function kernelReplayRun(input: Uint8Array, encode: boolean): Uint8Array {
    const ex = engine();
    if (input.length > ex.wasm_replay_io_cap()) throw new Error('replay: capacity exceeded');
    const base = ex.wasm_replay_io_ptr();
    mem(ex).set(input, base);
    const r = encode ? ex.wasm_replay_encode(input.length) : ex.wasm_replay_decode(input.length);
    if (r < 0) throw replayError(r, ex.wasm_replay_error_detail());
    return mem(ex).slice(base, base + r);
}

export function kernelReplayEncode(input: Uint8Array): Uint8Array {
    return kernelReplayRun(input, true);
}

export function kernelReplayDecode(integerBytes: Uint8Array): Uint8Array {
    return kernelReplayRun(integerBytes, false);
}

// Worst-case wire bytes for one exported move: type + n + 2 x MAX_MOVE_CARDS
// wire cards (cnitro's wasm build pins MAX_MOVE_CARDS=28). The chunk size
// derives from the kernel's actual IO capacity so the two can't drift apart;
// the kernel clamps defensively on its side too (wasm_export_moves), and the
// loop below advances by the RETURNED count, so even a mismatched clamp can
// shorten chunks but never skip moves.
const MOVE_WIRE_MAX = 2 + 2 * 28;

export function kernelLegalMoves(game: Game, player_id: string): { type: string; cards?: Card[]; attack_cards?: Card[] }[] {
    const seat = game.players.findIndex(p => p.player_id === player_id);
    if (seat < 0) return [];
    const ex = engine();
    // Enumeration is a state READER: force a fresh marshal so it can never
    // consume a resident mark left by a prior decision on a since-mutated game
    // object (see __setResident). Only the action executing a just-chosen move
    // may skip the marshal.
    residentFor = null;
    marshalGame(ex, game);
    const total = ex.wasm_legal_moves(seat);
    const base = ex.wasm_io_ptr();
    const chunk = Math.floor((ex.wasm_io_cap() - 4) / MOVE_WIRE_MAX);
    const moves: { type: string; cards?: Card[]; attack_cards?: Card[] }[] = [];
    for (let start = 0; start < total;) {
        ex.wasm_export_moves(start, chunk);
        const buf = mem(ex);
        let q = base;
        const n = buf[q] | (buf[q + 1] << 8) | (buf[q + 2] << 16) | (buf[q + 3] << 24); q += 4;
        for (let i = 0; i < n; i++) {
            const type = MOVE_TYPE[buf[q++]];
            const k = buf[q++];
            if (type === 'pickup' || type === 'good' || type === 'wait') {
                q += k * 2;
                moves.push({ type });
                continue;
            }
            const cards: Card[] = new Array(k);
            for (let j = 0; j < k; j++) cards[j] = cardFromWire(buf[q++]);
            if (type === 'cover') {
                const attacks: Card[] = new Array(k);
                for (let j = 0; j < k; j++) attacks[j] = cardFromWire(buf[q++]);
                moves.push({ type, cards, attack_cards: attacks });
            } else {
                q += k;
                moves.push({ type, cards });
            }
        }
        if (n <= 0) break;   // defensive: a zero-move chunk must not spin
        start += n;
    }
    return moves;
}

// ---------------------------------------------------------------------------
// Shared with the bot-module bridge (bots.ts)
// ---------------------------------------------------------------------------
// bots.wasm embeds this same kernel (plus every algorithmic bot strategy), so
// its bridge reuses this module's byte-layout marshaling instead of
// duplicating it. Underscore-prefixed: internal surface, not part of the
// kernel API the actions/ handlers use.
export type { EngineExports };
export { decodeBase64 as __decodeBase64, marshalGame as __marshalGame, mem as __mem };
// Map, not a plain object: the per-log lookup in the bots bridge showed up
// as a megamorphic keyed load (~10% of the heuristic-bot pipeline profile).
export const __LOG_TYPE_TO_INT: Map<string, number> = new Map(
    LOG_TYPE_FROM_INT.map((t, i) => [t, i]),
);
export { pooledCard as __pooledCard, MOVE_TYPE as __MOVE_TYPE };
export { wireLogCard as __wireLogCard, cardFromWire as __cardFromWire };
export { LOG_TYPE_FROM_INT as __LOG_TYPE_FROM_INT };
