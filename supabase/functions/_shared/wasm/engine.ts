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
import { RULES_WASM_B64 } from './rules_wasm.ts';

// ---------------------------------------------------------------------------
// Instantiation
// ---------------------------------------------------------------------------

interface EngineExports {
    memory: WebAssembly.Memory;
    wasm_init(): void;
    wasm_set_seed(s: number): void;
    wasm_reject_reason(): number;
    wasm_io_ptr(): number;
    wasm_io_cap(): number;
    wasm_cards_a_ptr(): number;
    wasm_cards_b_ptr(): number;
    wasm_import_state(): void;
    wasm_export_state(): number;
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

function engine(): EngineExports {
    if (exportsCache) return exportsCache;
    const module = new WebAssembly.Module(decodeBase64(RULES_WASM_B64) as BufferSource);
    const instance = new WebAssembly.Instance(module, {});
    const ex = instance.exports as unknown as EngineExports;
    ex.wasm_init();
    exportsCache = ex;
    return ex;
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
// so the action logs from zero exactly like after wasm_import_state). ANY
// marshal clears the mark, so the window is strictly choose -> next kernel
// call on the same object.
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
}

interface KernelState {
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

interface KernelLog {
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
function appendLogs(game: Game, kernelLogs: KernelLog[], preFlipped: Card | null, postFlipped: Card | null): void {
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

function runKernel(game: Game, action: KernelAction): KernelRun {
    const ex = engine();
    marshalGame(ex, game);
    // Draws consume kernel randomness; reseed per call so play stays as
    // unpredictable as the Math.random() the TS engine used.
    ex.wasm_set_seed(seedSource ? (seedSource() >>> 0) : ((Math.random() * 0xffffffff) >>> 0));

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

// refillPlayerHandsWithEvents compatibility: run the kernel refill and return
// the same { refillEvents, drawLogs } shape the TS implementation produced.
export function kernelRefill(game: Game): { refillEvents: AnimationEvent[]; drawLogs: { player_id: string; cards: Card[] }[] } {
    const preFlipped = game.flipped;
    const run = runKernel(game, { kind: 'refill' });
    const events = buildEvents(game, run, {});
    applyStateToGame(game, run.post!, null);
    const flippedWasDrawn = preFlipped !== null && game.flipped === null;
    const drawLogs = run.logs
        .filter(l => l.type === LOG_TYPE.DRAW)
        .map(l => ({
            player_id: game.players[l.playerIdx].player_id,
            cards: l.pairs.map(p =>
                flippedWasDrawn && sameCard(p.primary, preFlipped!) ? p.primary : { suit: -1, value: -1 }),
        }));
    return { refillEvents: events, drawLogs };
}

// Kernel-side counterparts of the thin TS projections that stayed in
// common_utils/pure_bot_actions for the client's synchronous use (canCover,
// game_done, get_next_player_index, shouldBotActCore). Exported so tests can
// police that the TS copies never drift from the kernel.
export function kernelGameDone(game: Game): string | null {
    const ex = engine();
    marshalGame(ex, game);
    const loser = ex.wasm_game_done();
    return loser >= 0 ? game.players[loser].player_id : null;
}

export function kernelShouldAct(game: Game, player_id: string): boolean {
    const seat = game.players.findIndex(p => p.player_id === player_id);
    if (seat < 0) return false;
    const ex = engine();
    marshalGame(ex, game);
    return ex.wasm_should_act(seat) !== 0;
}

export function kernelNextPlayer(game: Game, current: number): number {
    const ex = engine();
    marshalGame(ex, game);
    return ex.wasm_next_player(current);
}

export function kernelCanCover(attack: Card, defense: Card, powerSuit: number): boolean {
    return engine().wasm_can_cover(attack.suit, attack.value, defense.suit, defense.value, powerSuit) !== 0;
}

// Worst-case wire bytes for one exported move: type + n + 2 x MAX_MOVE_CARDS
// wire cards (cnitro's wasm build pins MAX_MOVE_CARDS=40). The chunk size
// derives from the kernel's actual IO capacity so the two can't drift apart;
// the kernel clamps defensively on its side too (wasm_export_moves).
const MOVE_WIRE_MAX = 2 + 2 * 40;

export function kernelLegalMoves(game: Game, player_id: string): { type: string; cards?: Card[]; attack_cards?: Card[] }[] {
    const seat = game.players.findIndex(p => p.player_id === player_id);
    if (seat < 0) return [];
    const ex = engine();
    marshalGame(ex, game);
    const total = ex.wasm_legal_moves(seat);
    const base = ex.wasm_io_ptr();
    const chunk = Math.floor((ex.wasm_io_cap() - 4) / MOVE_WIRE_MAX);
    const moves: { type: string; cards?: Card[]; attack_cards?: Card[] }[] = [];
    for (let start = 0; start < total; start += chunk) {
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
