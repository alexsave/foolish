// TypeScript bridge to bots.wasm, the kernel every host runs (c/src plus every
// bot strategy and the wasm entries in c/wasm/).
//
// The game itself is never marshalled from here: servers and tests hold a game
// as the C Table (sdk/ts/table/server_table.ts) and the web as its client slot
// (sdk/ts/table/client_table.ts), both over this module's instance. What is left
// here are the doors that take a handful of bytes rather than a board: the bot
// roster, the replay codec and its extras and links, the iMessage envelope, the
// one-tap cover resolver and the animation rules. Loaded lazily and cached.

import { loadWasmGz, loadWasmGzAsync } from './wasm_asset.ts';
import { LAYOUT_HASH as BOTS_LAYOUT_HASH } from '../gen/layout_hash.bots.ts';
import { assertLayoutHash } from './layout_hash.ts';
import { memOf as viewMemOf, readTableView, readReplaySummary, readReplayError, readReplayFrameIndex, TableView_Snap, ReplaySummary_Snap, ReplayError_Snap } from '../gen/view_layout.bots.ts';
import { memOf as msgMemOf, readMsgHeader, writeMsgHeader, readReplayExtras, writeReplayExtras,
         MSG_MAX_NAME, REPLAY_EXTRAS_FLAG_NAMES, REPLAY_EXTRAS_FLAG_TIMES, REPLAY_LINK_STYLE_URL, REPLAY_LINK_STYLE_QR,
         type MsgHeader_Snap, type ReplayExtras_Snap } from '../gen/msg_layout.bots.ts';
import * as A from '../gen/anim.bots.ts';
import { memOf as animMemOf } from '../gen/anim.bots.ts';

/** A card as the kernel's doors here read and write it (suit 0..3, value 1..13; -1/-1 is a hidden card). */
export interface Card { readonly suit: number; readonly value: number }

/** The exports of bots.wasm this bridge calls. */
interface EngineExports {
    memory: WebAssembly.Memory;
    wasm_init(): void;
    wasm_layout_hash(): number;
    wasm_io_ptr(): number;
    wasm_io_cap(): number;
    wasm_cards_a_ptr(): number;
    wasm_cards_b_ptr(): number;
    wasm_legal_moves(i: number): number;
    wasm_legal_moves_ptr(): number;
    wasm_replay_io_ptr(): number;
    wasm_replay_io_cap(): number;
    wasm_replay_error_ptr(): number;
    wasm_msg_decode?(len: number): number;
    wasm_msg_seal?(): number;
    wasm_msg_header_ptr?(): number;
    wasm_msg_rule_p?(aLen: number, bLen: number): number;
    wasm_msg_rebase?(pendingRound: number, seat: number, wireLen: number): number;
    wasm_msg_pickup_hold?(seat: number, sentAt: number, now: number): number;
}

// One cached view over the whole linear memory. The kernel never mallocs, so
// the buffer identity is stable, but a detached buffer is re-viewed anyway.
let memView = new Uint8Array(0);
function mem(ex: { memory: WebAssembly.Memory }): Uint8Array {
    if (memView.buffer !== ex.memory.buffer) memView = new Uint8Array(ex.memory.buffer);
    return memView;
}

// 1-byte wire cards (c/wasm/wire.h): 0..51 = suit*13+(value-1), 0xFE the hidden
// card, 0xFF no card. A hostile suit or value wraps through int8 and is clamped
// the way the kernel clamps it (suit 0..3, value 1..13).
const WIRE_HIDDEN = 0xfe;
const WIRE_NONE = 0xff;
const i8 = (v: number) => (v > 127 ? v - 256 : v);
const HIDDEN_CARD: Card = { suit: -1, value: -1 };
const CARD_POOL: Card[] = [];
for (let s = 0; s < 4; s++) for (let v = 1; v <= 13; v++) CARD_POOL[s * 13 + v - 1] = { suit: s, value: v };
function wireStateCard(c: Card): number {
    let s = i8(c.suit & 0xff), v = i8(c.value & 0xff);
    if (s === -1 && v === -1) return WIRE_HIDDEN;
    if (s < 0) s = 0; else if (s > 3) s = 3;
    if (v < 1) v = 1; else if (v > 13) v = 13;
    return s * 13 + (v - 1);
}
function wireLogCard(c: Card | null | undefined): number {
    return c ? wireStateCard(c) : WIRE_NONE;
}
function cardFromWire(b: number): Card {
    return b === WIRE_HIDDEN ? HIDDEN_CARD : CARD_POOL[b <= 51 ? b : 51];
}

const MOVE_TYPE = ['attack', 'cover', 'pass', 'pickup', 'good', 'wait'];

// The legal moves of whatever game is RESIDENT in the kernel, no marshal and no
// copy: wasm_legal_moves fills the kernel's own LegalMoves and this reads it
// where it lies, through the generated accessors (sdk/ts/gen/anim.bots.ts).
function residentLegalMoves(ex: EngineExports, seat: number): { type: string; cards?: Card[]; attack_cards?: Card[] }[] {
    const total = ex.wasm_legal_moves(seat);
    const m = animMemOf(ex.memory.buffer);
    const lm = ex.wasm_legal_moves_ptr();
    const card = (p: number): Card => ({ suit: A.Card_get_suit(m, p), value: A.Card_get_value(m, p) });
    const moves: { type: string; cards?: Card[]; attack_cards?: Card[] }[] = new Array(total);
    for (let i = 0; i < total; i++) {
        const mv = A.LegalMoves_moves_at(lm, i);
        const type = MOVE_TYPE[A.LegalMove_get_type(m, mv)];
        const k = A.LegalMove_get_n_cards(m, mv);
        if (type === 'pickup' || type === 'good' || type === 'wait') { moves[i] = { type }; continue; }
        const cards: Card[] = new Array(k);
        for (let j = 0; j < k; j++) cards[j] = card(A.LegalMove_cards_at(mv, j));
        if (type !== 'cover') { moves[i] = { type, cards }; continue; }
        const attacks: Card[] = new Array(k);
        for (let j = 0; j < k; j++) attacks[j] = card(A.LegalMove_attack_cards_at(mv, j));
        moves[i] = { type, cards, attack_cards: attacks };
    }
    return moves;
}

// The session log record types (c/src/game.h LOG_*), for the desync message only.
const LOG_TYPE_NAMES = ['game_start', 'attack', 'cover', 'pass', 'pickup', 'good', 'discard', 'defender_change', 'player_out', 'draw'];

// Mirrors REPLAY_E* in replay.h. The NUMBERS are the wire between the two
// files, so 3, 8 and 9 are a deliberate hole: they belonged to the retired
// retrodiction format and are unreachable now.
//
// `d` is the refusal's parameters (replay.h ReplayError) as the generated reader
// copies them out; zeros stand in when a caller has no module to ask (a message
// written from the code alone).
const NO_REPLAY_DETAIL: ReplayError_Snap = { version: 0, logType: 0, menu: 0 };

/** Why the module's last replay call refused, through the generated reader. */
function replayErrorDetail(ex: EngineExports): ReplayError_Snap {
    return readReplayError(viewMemOf(ex.memory.buffer), ex.wasm_replay_error_ptr());
}

export function __replayError(negCode: number, d: ReplayError_Snap = NO_REPLAY_DETAIL): Error {
    switch (-negCode) {
        case 1: return new Error(`unsupported replay format version ${d.version}`);
        case 2: return new Error('invalid replay: leftover data after game end');
        case 4: return new Error('replay guard: too many events');
        case 5: return new Error('replay desync: no legal moves');
        case 6: return new Error('replay desync: conservation');
        case 7: return new Error('replay desync: played card not in hand');
        case 10: return new Error('replay desync: the unseen pool is empty');
        case 11: return new Error('replay desync: a supplied reveal is not unseen');
        case 12: return new Error(
            `replay desync: logged ${LOG_TYPE_NAMES[d.logType] ?? d.logType} not in menu of ${d.menu}`);
        case 13: return new Error('replay desync: round end not in menu');
        case 14: return new Error('replay desync: attack continuation');
        case 15: return new Error('replay desync: pass continuation');
        case 16: return new Error('incomplete replay: the actions ran out before the coded atom count');
        case 17: return new Error('replay desync: logs continue after the game ended');
        case 18: return new Error('empty menu');
        case 19: return new Error('encode: chosen index out of range');
        // REPLAY_EHEADER covers every "the header does not fit the game" fault,
        // including a rebuilt deal that disagrees with the header's opening seat.
        case 20: return new Error('invalid replay header (trump not in alphabet, or the rebuilt deal contradicts it)');
        case 21: return new Error('replay: malformed encode input');
        case 22: return new Error('replay: capacity exceeded');
        // A refusal, not a fault - see REPLAY_ETOOLONG in c/src/replay.h.
        case 23: return new Error('replay: game too long to encode (session log overflowed)');
        default: return new Error(`replay kernel error ${negCode}`);
    }
}

interface BotsExports extends EngineExports {
    wasm_replay_encode_v6_from_game(max_atoms: number): number;
    wasm_replay_events(viewer: number, from: number, code_len: number): number;
    wasm_replay_events_index_ptr(): number;
    wasm_replay_step_count(code_len: number): number;
    wasm_replay_step_index(code_len: number): number;
    wasm_replay_step_masked_state(code_len: number, step: number, viewer: number): number;
    wasm_replay_step_logs(code_len: number, step: number): number;
    wasm_replay_summary(code_len: number): number;
    wasm_replay_extras_encode(): number;
    wasm_replay_extras_decode(blob_len: number, player_count: number, move_count: number): number;
    wasm_replay_extras_ptr(): number;
    wasm_replay_link(moves_len: number, style: number): number;
    wasm_replay_b32_encode(in_len: number): number;
    wasm_replay_b32_decode(in_len: number): number;
    wasm_replay_link_parse(in_len: number): number;
    wasm_bot_roster_dump(): number;
    // Belief probe (observability; off until reset arms it)
    // Animation core (c/src/anim_plan.h) — the platform-independent animation
    // policy the web pure modules (src/state/*) delegate to. bots-only.
    wasm_anim_should_drop_stale(hasLast: number, last: number, hasIncoming: number, incoming: number): number;
    wasm_anim_stale_optimistic(nOpt: number, nTable: number, nNamed: number): number;
    wasm_anim_event_key(type: number, suit: number, value: number, from: number, to: number, seat: number): number;
    wasm_anim_finish_rows(nElim: number, gameOver: number, nPlayers: number, mySeat: number): number;
    wasm_anim_hand_laid_out(nCards: number, nOrder: number, deferredLo: number, deferredHi: number): number;
    wasm_anim_conflict_verdicts(pendingAttacks: number, defenderHand: number,
                                finalUncovered: number): number;
    wasm_anim_set_transport(transport: number): number;
    wasm_anim_transport(): number;
    // The plan, the frame and the beats: the structs are read where they lie.
    wasm_anim_plan_ptr(): number;
    wasm_anim_frame_ptr(): number;
    wasm_anim_beats_ptr(): number;
    wasm_anim_build_plan(nEvents: number, nPlayers: number, finalDeck: number,
                         finalDiscard: number, finalFlipped: number): number;
    wasm_anim_plan_at(nowMs: number): number;
    wasm_anim_build_beats(nEvents: number): number;
    wasm_anim_reversal_order(): number;
}

// ANIM_TRANSPORT_* (c/src/anim_plan.h).
export const ANIM_TRANSPORT_UNSET = 0;
export const ANIM_TRANSPORT_CHAIN = 1;
export const ANIM_TRANSPORT_SERVER = 2;

/** Which transport this module is set to, for diagnostics: a wrong-mode bug
 *  looks exactly like an animation bug. The setter is deliberately not exported
 *  as a public knob - a host states its transport at init, in `bots()`. */
export function animTransport(): number { return bots().wasm_anim_transport(); }

/** THE ONE PLACE A TEST MAY MOVE IT. The two transports have to be provable
 *  against the same input in the same process (the FMSG suites already drive
 *  chain behaviour through this module), and that cannot be done without
 *  putting it back afterwards. */
export function __setAnimTransport(transport: number): void {
    const rc = bots().wasm_anim_set_transport(transport);
    if (rc < 0) throw new Error(`anim_set_transport error ${rc}`);
}

/** One row of the kernel's bot roster (c/src/bot_roster.c ROSTER). */
export interface BotRosterEntry {
    key: string;
    /** STRAT_* brain id (strategy.h). */
    strat: number;
    /** Belief bot: the session log must be hydrated before it chooses. */
    usesLogs: boolean;
    /** This build links the brain, so it can actually be run here. */
    linked: boolean;
    /** Seeded as a live bot on the site (seed.sql). */
    seeded: boolean;
    /** Shown in the offline picker. */
    offline: boolean;
    /** Strength order, 1 = weakest; 0 = unranked. */
    tier: number;
}

let rosterCache: BotRosterEntry[] | null = null;

/** The kernel's bot roster (bot_roster.h): hosts look it up, never restate it. */
export function kernelBotRoster(): BotRosterEntry[] {
    if (rosterCache) return rosterCache;
    const ex = bots();
    const n = ex.wasm_bot_roster_dump();
    if (n < 0) throw new Error('bot_roster: the kernel refused to dump the table');
    const buf = mem(ex);
    let q = ex.wasm_io_ptr();
    const out: BotRosterEntry[] = [];
    for (let i = 0; i < n; i++) {
        const linked = buf[q++] !== 0, strat = buf[q++], usesLogs = buf[q++] !== 0;
        const seeded = buf[q++] !== 0, offline = buf[q++] !== 0, tier = buf[q++];
        const klen = buf[q++];
        let key = '';
        for (let k = 0; k < klen; k++) key += String.fromCharCode(buf[q++]);
        out.push({ key, strat, usesLogs, linked, seeded, offline, tier });
    }
    rosterCache = out;
    return out;
}

/**
 * Brain ids by roster key - a lookup, not a mirrored table, so an unknown key
 * is -1 rather than a stale number. Lazy: reading a property instantiates
 * bots.wasm, and a lobby-only cold start must not pay that for an import.
 */
export const STRAT: Record<string, number> = new Proxy({} as Record<string, number>, {
    get: (_t, key) => (typeof key === 'string' ? kernelBotStrat(key) : undefined),
    has: (_t, key) => typeof key === 'string' && kernelBotStrat(key) >= 0,
    ownKeys: () => kernelBotRoster().map((e) => e.key),
    getOwnPropertyDescriptor: (_t, key) => (typeof key === 'string' && kernelBotStrat(key) >= 0
        ? { enumerable: true, configurable: true, value: kernelBotStrat(key) }
        : undefined),
});

/** The STRAT_* brain id the kernel's roster gives this key, or -1. */
export function kernelBotStrat(key: string): number {
    return kernelBotRoster().find((e) => e.key === key)?.strat ?? -1;
}

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

/** Instantiate bots.wasm now (a harness that measures memory or timing before its first call). */
export function __ensureBots(): void { bots(); }

/**
 * Prepare bots.wasm where it cannot be read synchronously — i.e. the browser,
 * which has no filesystem and must FETCH the .gz. Await this once before any
 * bots() call; on the server it is a no-op fast path (fs is synchronous).
 *
 * The browser needs the big module for two independent reasons, and both landed
 * the same day:
 *   * FMSG - the iMessage envelope - seals from a resident session log, and /m/
 *     is a web page; and
 *   * A5 - replaying a shared code rebuilds the game and plays it through the
 *     real engine (replay_steps.c).
 * One module behind every host is the steer (A10). This is the web's way in.
 *
 * Deliberately a fetched ASSET rather than a base64 twin of the same bytes: a
 * second carrier of one kernel goes stale while the other is rebuilt, and 80 KB
 * of single-line base64, rewritten on every kernel build, is miserable in git.
 * The cost is that the browser's door is async; callers await this first.
 */
export async function ensureBotsAsync(): Promise<void> {
    if (exportsCache) return;
    await loadWasmGzAsync('bots');   // caches the inflated bytes for bots()
    bots();
}

/** The warm module's exports, for the web client's slot (sdk/ts/table/client_table.ts), which types what it calls. */
export function __clientKernelExports(): WebAssembly.Exports {
    return bots() as unknown as WebAssembly.Exports;
}

function bots(): BotsExports {
    if (exportsCache) return exportsCache;
    const module = new WebAssembly.Module(loadWasmGz('bots') as BufferSource);
    const instance = new WebAssembly.Instance(module, {});
    const ex = instance.exports as unknown as BotsExports;
    assertLayoutHash('bots.wasm', ex, BOTS_LAYOUT_HASH, 'sdk/ts/gen/layout_hash.bots.ts');
    ex.wasm_init();
    // THE TRANSPORT, said once (anim_plan.h). Every host that reaches the
    // kernel through this module is the server shape: a card's confirmation is
    // its own later broadcast, so "the newest news does not mention my card"
    // means the receipt is in the post, not that it was rejected. The kernel
    // has no default for this and errors rather than guessing, which is what
    // keeps a new host from silently inheriting iMessage's answer.
    ex.wasm_anim_set_transport(ANIM_TRANSPORT_SERVER);
    exportsCache = ex;
    return ex;
}

// ---------------------------------------------------------------------------
// Belief probe: what a bot's search read (c/wasm/wasm_bots_api.c)
// ---------------------------------------------------------------------------

// ---------- FMSG: the iMessage envelope (c/src/msg_wire.h) -------------
//
// An iMessage game has no server: the whole game is (32-byte deal seed, v6
// replay code) in an MSMessage URL, and every device rebuilds it by re-dealing
// from the seed and replaying the code through the kernel. These two calls are
// the only way in — the envelope's layout lives in C, once, so the phone and
// the web can never read the same bytes as different games.
//
// Both ride the REPLAY io buffer, never g_io: an FMSG call IS a replay call (its
// body is a v6 code), so an envelope in g_io would be clobbered by the codec's
// own bignum scratch mid-decode. See wasm_api.c.

// The header crosses as the C struct itself (msg_wire.h MsgHeader), read and
// written through the generated snapshot reader and writer
// (sdk/ts/gen/msg_layout.bots.ts) at wasm_msg_header_ptr. Nothing here knows an
// offset or a cap: MSG_MAX_NAME comes from the same generated module, and a name
// too long for the struct's slot is the writer's RangeError.
export { MSG_MAX_NAME };

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
    /// THE FOOL'S PENALTY (c/src/msg_wire.h format 4). `opening` is the seat
    /// this deal opens on, or 0xFF (MSG_NO_OPENING) for the ordinary
    /// lowest-trump rule; `carry_key`/`carry_fool` are a WAITING lobby's
    /// rematch carry, 0 / 0xFF when there is none. All three go BOTH ways -
    /// they are terms of the deal, not claims about the body, so a caller that
    /// writes them seals a format-4 envelope.
    opening: number;
    carry_key: number;
    carry_fool: number;
    joins: MsgJoin[];
}

/// The wire's "no penalty" sentinels, so callers never spell 0xFF themselves.
export const MSG_NO_OPENING = 0xff;
export const MSG_NO_FOOL = 0xff;

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

// The struct at wasm_msg_header_ptr <-> this bridge's MsgEnvelope. Only the
// shapes differ (the snapshot holds byte arrays as number[] and camelCases its
// fields); every offset is the generated module's.
function readHeader(ex: EngineExports): MsgEnvelope {
    const s = readMsgHeader(msgMemOf(ex.memory.buffer), ex.wasm_msg_header_ptr!());
    return {
        format: s.e.format, flags: s.e.flags, phase: s.e.phase, n_players: s.e.nPlayers,
        variant: s.e.variant, round: s.e.round, last_actor_seat: s.e.lastActorSeat,
        game_id: s.e.gameId, turn: s.e.turn,
        parent8: Uint8Array.from(s.e.parent8),
        seed: Uint8Array.from(s.e.seed),
        digest: Uint8Array.from(s.digest),
        sent_at: s.e.sentAt, n_new: s.e.nNew, opening: s.e.opening,
        carry_key: s.e.carryKey, carry_fool: s.e.carryFool,
        joins: s.e.joins.map((j) => ({ seat: j.seat, name: j.name })),
    };
}

function writeHeader(ex: EngineExports, e: MsgEnvelope): void {
    const snap: MsgHeader_Snap = {
        e: {
            format: e.format, flags: e.flags, phase: e.phase, nPlayers: e.n_players,
            variant: e.variant, round: e.round, lastActorSeat: e.last_actor_seat,
            gameId: e.game_id, turn: e.turn,
            parent8: Array.from(e.parent8.subarray(0, 8)),
            seed: Array.from(e.seed.subarray(0, 32)),
            sentAt: e.sent_at & 0xffff, nNew: e.n_new, opening: e.opening,
            carryKey: e.carry_key >>> 0, carryFool: e.carry_fool,
            joins: e.joins.map((j) => ({ seat: j.seat, name: j.name })),
        },
        digest: Array.from(e.digest.subarray(0, 32)),
    };
    writeMsgHeader(msgMemOf(ex.memory.buffer), ex.wasm_msg_header_ptr!(), snap);
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
    mem(ex).set(envelope, ex.wasm_replay_io_ptr());
    const r = ex.wasm_msg_decode(envelope.length);
    if (r < 0) throw msgError(r);
    return readHeader(ex);
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
    mem(ex).set(a, base);
    mem(ex).set(b, base + a.length);
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
    return residentLegalMoves(bots() as unknown as EngineExports, seat);
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
    mem(ex).set(wire, ex.wasm_cards_a_ptr());
    const r = ex.wasm_msg_rebase(pendingRound, seat, wire.length);
    if (r < 0) throw msgError(r);
    return r;
}

// The PUBLIC view of the game the last kernelMsgDecode adopted — every hand as
// backs, the deck masked. What /m/ renders for a stranger with a link, and what
// the bubble snapshot shows (it lands in notifications and on lock screens, so
// it must never carry a hand — design §5 invariants).
//
// The resident game is read into the web client's slot as a spectator sees it
// (c/src/client_table.h client_adopt_board), so this needs no re-deserialize:
// the envelope already put the game in the kernel. The masking itself is in
// view.c, like every other view in the product — nothing here decides what a
// stranger may see. The view names no one: the joins carry the names.
export function kernelMsgPublicView(): { view: TableView_Snap } {
    const ex = bots() as unknown as {
        memory: WebAssembly.Memory; wasm_client_adopt_resident(viewer: number): number; wasm_client_view_ptr(): number;
    };
    const rc = ex.wasm_client_adopt_resident(-1);
    if (rc !== 0) throw new Error(`view: the resident game does not read (${rc})`);
    return { view: readTableView(viewMemOf(ex.memory.buffer), ex.wasm_client_view_ptr()) };
}

/* ---------------- the replay code's extras blob (c/src/replay_extras.h) ------
 *
 * The nicknames and per-move timing behind the dash in a share link. The codec
 * is the kernel's - one encoder, shared with the phone (fio_replay_extras_link)
 * - and these are the doors the web and the server reach it through. Both are
 * synchronous and assume a warm module, like every other reader here.
 */


/** The kernel's REPLAY_EXTRAS_E* codes, as the messages this format has always
 *  thrown - callers catch extras failures and fall back to "P1"/"P2". */
function __extrasError(code: number): Error {
    switch (-code) {
        case 1: return new Error('extras: truncated header');
        case 2: return new Error('extras: unsupported version');
        case 3: return new Error('extras: unterminated name');
        case 4: return new Error('extras: truncated time header');
        case 5: return new Error('extras: too few gaps for the moves they describe');
        case 6: return new Error('extras: malformed argument blob');
        case 7: return new Error('extras: exceeds the kernel IO buffer');
        default: return new Error(`extras: kernel error ${code}`);
    }
}

/**
 * The names and timing into the kernel's own struct (replay_extras.h
 * ReplayExtras) at wasm_replay_extras_ptr, through the generated writer. The
 * codec's packed argument blob is built from it IN C (replay_extras_pack), so
 * nothing here knows its layout; a name too wide for the struct's slot, or more
 * gaps than it holds, is the writer's RangeError.
 */
function __extrasArgs(ex: BotsExports, names: string[] | null, startTime: number | null,
                      gaps: number[] | null): void {
    const hasNames = (names?.length ?? 0) > 0;
    const hasTimes = startTime !== null && gaps !== null;
    const snap: ReplayExtras_Snap = {
        flags: (hasNames ? REPLAY_EXTRAS_FLAG_NAMES : 0) | (hasTimes ? REPLAY_EXTRAS_FLAG_TIMES : 0),
        names: hasNames ? names!.map((text) => ({ text })) : [],
        startTime: hasTimes ? startTime! : 0,
        gaps: hasTimes ? gaps! : [],
    };
    writeReplayExtras(msgMemOf(ex.memory.buffer), ex.wasm_replay_extras_ptr(), snap);
}

/**
 * Roster + timing -> the raw extras blob. `names` must be as wide as the table
 * (a reader takes the seat count from the decoded moves); unnamed seats are ''.
 * Passing null for either half leaves that section out.
 */
export function kernelReplayExtrasEncode(names: string[] | null, startTime: number | null,
                                         gaps: number[] | null): Uint8Array {
    const ex = bots();
    __extrasArgs(ex, names, startTime, gaps);
    const n = ex.wasm_replay_extras_encode();
    if (n < 0) throw __extrasError(n);
    const base = ex.wasm_io_ptr();
    return mem(ex).slice(base, base + n);
}

/** What a decoded extras blob says. */
export interface KernelReplayExtras {
    names: string[] | null;
    startTime: number | null;
    moveGaps: number[] | null;
}

/**
 * The raw extras blob -> roster + timing. `playerCount` and `moveCount` come
 * from the DECODED MOVES: the blob carries neither, and reading it needs both.
 * Throws on a malformed blob.
 */
export function kernelReplayExtrasDecode(blob: Uint8Array, playerCount: number,
                                         moveCount: number): KernelReplayExtras {
    const ex = bots();
    if (blob.length > ex.wasm_replay_io_cap()) throw new Error('extras: blob exceeds the kernel IO buffer');
    mem(ex).set(blob, ex.wasm_replay_io_ptr());
    const rc = ex.wasm_replay_extras_decode(blob.length, playerCount, moveCount);
    if (rc < 0) throw __extrasError(rc);
    const x = readReplayExtras(msgMemOf(ex.memory.buffer), ex.wasm_replay_extras_ptr());
    const hasTimes = (x.flags & REPLAY_EXTRAS_FLAG_TIMES) !== 0;
    return {
        names: (x.flags & REPLAY_EXTRAS_FLAG_NAMES) ? x.names.map((n) => n.text) : null,
        startTime: hasTimes ? x.startTime : null,
        moveGaps: hasTimes ? [...x.gaps] : null,
    };
}

/**
 * THE WHOLE SHAREABLE LINK for a finished game: `https://foolish.cards/<moves>`
 * plus `-<base32 extras>` when the roster says anything, and the bare link when
 * nobody at the table is named. Built in the kernel rather than concatenated
 * here (c/src/replay_extras.h) - the prefix is a constant and the dash is the
 * codec's, so a phone, a watch and a browser have no business each writing it.
 * `names` must be as wide as the table; unnamed seats are ''.
 */
/** replay_extras.h REPLAY_LINK_STYLE_*, from the kernel: the https link, or the QR form. */
export const REPLAY_LINK = { url: REPLAY_LINK_STYLE_URL, qr: REPLAY_LINK_STYLE_QR } as const;

// base32, from the kernel (replay.c replay_b32_encode/decode). replay.h called
// this alphabet "the web's codec.ts alphabet" and replay.c said a code made on
// the web "reads here byte for byte" - a mirror documenting itself as one. The
// web asks for it now, so there is one alphabet.
export function kernelB32Encode(bytes: Uint8Array): string {
    const ex = bots();
    if (bytes.length >= ex.wasm_replay_io_cap()) throw new Error('b32: input exceeds the kernel IO buffer');
    mem(ex).set(bytes, ex.wasm_replay_io_ptr());
    const w = ex.wasm_replay_b32_encode(bytes.length);
    if (w < 0) throw new Error('b32: encode overflowed the kernel IO buffer');
    const base = ex.wasm_io_ptr();
    return new TextDecoder().decode(mem(ex).slice(base, base + w));
}

// Accepts lower case, ignores characters outside the alphabet, and stops at the
// '-' where a share link's extras suffix begins. All of that is the kernel's
// decoder, not a convention restated here.
export function kernelB32Decode(code: string): Uint8Array {
    const ex = bots();
    const inBytes = new TextEncoder().encode(code);
    // The kernel writes a terminator one byte past the input.
    if (inBytes.length + 1 >= ex.wasm_replay_io_cap()) throw new Error('b32: input exceeds the kernel IO buffer');
    mem(ex).set(inBytes, ex.wasm_replay_io_ptr());
    const w = ex.wasm_replay_b32_decode(inBytes.length);
    if (w < 0) throw new Error('b32: decode overflowed the kernel IO buffer');
    const base = ex.wasm_io_ptr();
    return mem(ex).slice(base, base + w);
}

// The replay code out of whatever a person pasted - the kernel's
// replay_link_parse. Building a link and reading one back are two halves of the
// same format, and this is the half that has to REFUSE: replay_b32_decode is
// deliberately tolerant, so an unstripped "https" prefix would decode to a
// different game rather than fail. Throws when the input is not a code.
export function kernelReplayLinkParse(url: string): string {
    const ex = bots();
    const inBytes = new TextEncoder().encode(url);
    if (inBytes.length + 1 >= ex.wasm_replay_io_cap()) throw new Error('link: input exceeds the kernel IO buffer');
    mem(ex).set(inBytes, ex.wasm_replay_io_ptr());
    const w = ex.wasm_replay_link_parse(inBytes.length);
    if (w < 0) throw new Error(`not a replay code: ${JSON.stringify(url)}`);
    const base = ex.wasm_io_ptr();
    return new TextDecoder().decode(mem(ex).slice(base, base + w));
}

export function kernelReplayLink(moves: string, names: string[], style: number = REPLAY_LINK.url): string {
    const ex = bots();
    // The roster goes through the kernel's own struct; the moves code crosses as
    // itself (a base32 string the kernel NUL-terminates in place, since a long
    // game's code runs to tens of KB). The kernel writes that terminator one
    // byte past the input, so the code must fit the buffer with room for it.
    __extrasArgs(ex, names, null, null);
    const movesBytes = new TextEncoder().encode(moves);
    if (movesBytes.length >= ex.wasm_replay_io_cap()) throw new Error('extras: the replay code exceeds the kernel IO buffer');
    mem(ex).set(movesBytes, ex.wasm_replay_io_ptr());
    const w = ex.wasm_replay_link(movesBytes.length, style);
    if (w < 0) throw __extrasError(w);
    const base = ex.wasm_io_ptr();
    return new TextDecoder().decode(mem(ex).subarray(base, base + w));
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
    header: Omit<MsgEnvelope, 'digest' | 'turn' | 'round' | 'format' | 'sent_at' | 'n_new'
                              | 'opening' | 'carry_key' | 'carry_fool'>
          & { sent_at?: number; opening?: number; carry_key?: number; carry_fool?: number },
): Uint8Array {
    const ex = bots();
    if (!ex.wasm_msg_seal) throw new Error('kernelMsgSeal: module has no FMSG support');
    // format 2 here is a placeholder: msg_seal picks the real one off what the
    // header ends up carrying (a clock, or a bubble delta, seals format 3), so
    // the caller never states it twice. n_new is 0 for the same reason the
    // digest is: the kernel derives it, from the chain wasm_msg_decode adopted.
    writeHeader(ex, { ...header, sent_at: header.sent_at ?? 0, n_new: 0,
                      opening: header.opening ?? MSG_NO_OPENING,
                      carry_key: header.carry_key ?? 0,
                      carry_fool: header.carry_fool ?? MSG_NO_FOOL,
                      format: 2, turn: 0, round: 0, digest: new Uint8Array(32) });
    const r = ex.wasm_msg_seal();
    if (r < 0) throw msgError(r);
    const base = ex.wasm_replay_io_ptr();
    return mem(ex).slice(base, base + r);
}

// The best shareable REPLAY code for the game the last kernelMsgDecode
// adopted — the TS-side twin of MessageKernel.residentReplayCode()
// (sdk/swift/MessageEnvelope.swift), reached off the same resident g_game
// instead of Swift's fio_replay_share_code_b32. Used by the /m/ page's
// FINISHED-bubble funnel (docs/IMESSAGE_LOBBY_V3.md, batch 6 item B): once a
// payload decodes, msg_replay has already run the whole chain through the
// ORDINARY kernel handlers (handle_attack etc.), which log exactly like any
// other play — so the resident game already carries the full session log a
// v6 code needs. Only the envelope's own seed (env.seed, already decoded — no
// second kernel round-trip to fetch it) has to be supplied.
//
// It encodes whatever is already resident (wasm_replay_encode_v6_from_game), the
// log kernelMsgDecode built included, exactly as fio_replay_share_code_b32 does
// on the native/Swift side.
export function kernelResidentReplayCodeV6(seed: Uint8Array): Uint8Array {
    if (seed.length !== 32) {
        throw new Error(`replay: v6 needs a 32-byte deal seed, got ${seed.length}`);
    }
    const ex = bots();
    const base = ex.wasm_replay_io_ptr();
    mem(ex).set(seed, base);
    const n = ex.wasm_replay_encode_v6_from_game(1 << 30);
    if (n < 0) throw __replayError(n, replayErrorDetail(ex));
    return mem(ex).slice(base, base + n);
}

// ---------------------------------------------------------------------------
// Replay steps (docs/C_CORE_CONSOLIDATION.md F4.2 / A5)
//
// A v6 code, rebuilt into the real Game and replayed through the real engine,
// handed back as the SAME packed evwire frames live play broadcasts. The caller
// reads them with the client slot's push reader - the one it uses for live play -
// so a replay is not a second rendering path.
// ---------------------------------------------------------------------------

/** Steps a code replays to: the deal, then one per action. */
export function replayStepCount(code: Uint8Array): number {
    const ex = bots();
    mem(ex).set(code, ex.wasm_replay_io_ptr());
    const n = ex.wasm_replay_step_count(code.length);
    if (n < 0) throw __replayError(n, replayErrorDetail(ex));
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
    mem(ex).set(code, ex.wasm_replay_io_ptr());
    const len = ex.wasm_replay_step_index(code.length);
    if (len < 0) throw __replayError(len, replayErrorDetail(ex));
    const buf = mem(ex);
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
    // no-progress backstop, not a step limit. Where each frame lies in the chunk
    // is the kernel's own index (replay_steps.h ReplayFrameIndex), read through
    // the generated reader; the frames themselves are opaque bytes forwarded to
    // the same decoder live play feeds.
    for (let guard = 0; from < steps && guard < 4096; guard++) {
        mem(ex).set(code, ex.wasm_replay_io_ptr());
        const len = ex.wasm_replay_events(viewer, from, code.length);
        if (len < 0) throw __replayError(len, replayErrorDetail(ex));
        const index = readReplayFrameIndex(viewMemOf(ex.memory.buffer), ex.wasm_replay_events_index_ptr());
        if (index.nextStep <= from) throw new Error(`replay frames stalled at step ${from}/${steps}`);
        const buf = mem(ex);
        const base = ex.wasm_io_ptr();
        for (let i = 0; i < index.off.length; i++) {
            frames.push(buf.slice(base + index.off[i], base + index.off[i] + index.len[i]));
        }
        from = index.nextStep;
    }
    if (frames.length !== steps) {
        throw new Error(`replay produced ${frames.length} frames for ${steps} steps`);
    }
    return frames;
}

/** What a code says about its game as a whole (c/src/replay_steps.h ReplaySummary). */
export type ReplaySummary = ReplaySummary_Snap;

/**
 * A code at a glance, without playing it back: the seat count, the trump suit and
 * the opener, the fool (-1 for a code cut mid-game) and the order the others went
 * out, and how many moves its extras time. null when the code does not decode.
 */
export function replaySummary(code: Uint8Array): ReplaySummary | null {
    const ex = bots();
    mem(ex).set(code, ex.wasm_replay_io_ptr());
    const at = ex.wasm_replay_summary(code.length);
    return at > 0 ? readReplaySummary(viewMemOf(ex.memory.buffer), at) : null;
}

/**
 * The board action step `step` of a code was decided on, as `viewer` saw it: the
 * bytes a masked wasm_import_state reads, for the Oracle to import unchanged
 * (c/src/replay_steps.h replay_steps_board_v6). null when the step is not an
 * action or the viewer is not a seat.
 */
export function replayStepMaskedState(code: Uint8Array, step: number, viewer: number): Uint8Array | null {
    const ex = bots();
    mem(ex).set(code, ex.wasm_replay_io_ptr());
    const len = ex.wasm_replay_step_masked_state(code.length, step, viewer);
    if (len < 0) return null;
    const at = ex.wasm_io_ptr();
    return mem(ex).slice(at, at + len);
}

/**
 * The public log before action step `step`'s move, as the Oracle's memory: the
 * bytes wasm_import_logs reads (c/src/replay_steps.h replay_steps_memory_v6).
 * null when the step has no record of its own to pair with (a good, a round end).
 */
export function replayStepLogs(code: Uint8Array, step: number): Uint8Array | null {
    const ex = bots();
    mem(ex).set(code, ex.wasm_replay_io_ptr());
    const len = ex.wasm_replay_step_logs(code.length, step);
    if (len < 0) return null;
    const at = ex.wasm_io_ptr();
    return mem(ex).slice(at, at + len);
}

// ===========================================================================
// Animation core (c/src/anim_plan.h)
// ===========================================================================
// The bridge the web pure modules (src/state/*) delegate to, so the animation
// policy the React glitch-fixing hardened lives in C once — the same "one kernel
// behind every host" argument FMSG makes. bots.wasm is loaded at app boot
// (providers.tsx awaits ensureBotsAsync), and bots() is synchronous on the
// server, so these calls are safe synchronously in both.

// The pipeline's event-type and location NAMES, against the kernel's codes. The
// numbers are generated from anim_plan.h (sdk/ts/gen/anim.bots.ts); only the
// spelling of each name is TypeScript's, because the wire the web decodes from
// (src/state/pushSequence.ts) speaks strings.
export const ANIM_EVT: Record<string, number> = {
    magic_transition: A.ANIM_EVT_MAGIC_TRANSITION, deal: A.ANIM_EVT_DEAL, flipped: A.ANIM_EVT_FLIPPED,
    defender_move: A.ANIM_EVT_DEFENDER_MOVE, attack_pass: A.ANIM_EVT_ATTACK_PASS, cover: A.ANIM_EVT_COVER,
    pickup: A.ANIM_EVT_PICKUP, discard: A.ANIM_EVT_DISCARD, out: A.ANIM_EVT_OUT, refill: A.ANIM_EVT_REFILL,
    cards_to_trash: A.ANIM_EVT_CARDS_TO_TRASH, revert: A.ANIM_EVT_REVERT,
};
export const ANIM_LOC: Record<string, number> = {
    deck: A.ANIM_LOC_DECK, hand: A.ANIM_LOC_HAND, table: A.ANIM_LOC_TABLE,
    discard: A.ANIM_LOC_DISCARD, flipped: A.ANIM_LOC_FLIPPED,
};

/** The event-type string -> ANIM_EVT_* code (0 for an unknown/None type). */
export function animEventTypeCode(type: string | undefined): number {
    return (type && type in ANIM_EVT) ? ANIM_EVT[type] : 0;
}

/** The location string -> ANIM_LOC_* code; a location the wire did not name is
 *  ANIM_LOC_NONE, which is a code of its own and collides with no real place. */
export function animLocationCode(loc: string | undefined): number {
    return (loc && loc in ANIM_LOC) ? ANIM_LOC[loc] : A.ANIM_LOC_NONE;
}

/**
 * THE DEDUP KEY, in C (anim_plan.h anim_event_key): two events collide iff they
 * name the same (type, card, from, to, seat). The seat stands in for the player
 * id, because a plan is per viewer and the only actor whose prediction can
 * collide with a confirming broadcast is the local one.
 *
 * It is a plain number, not a string and not a BigInt: the kernel packs six
 * bytes, so every key it can make is exact in a double (asserted over the whole
 * range in c/tests/anim_plan_test.c). Nothing in TypeScript knows which byte is
 * which - a caller that wants a field back keeps the field, not the key.
 */
export function animEventKey(type: string | undefined, card: Card,
                             from: string | undefined, to: string | undefined,
                             seat: number | undefined): number {
    return bots().wasm_anim_event_key(
        animEventTypeCode(type), card.suit, card.value,
        animLocationCode(from), animLocationCode(to),
        seat === undefined ? A.ANIM_SEAT_NONE : seat);
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
    const buf = mem(ex);
    const base = ex.wasm_io_ptr();
    let p = base;
    for (const c of optCards) buf[p++] = wireStateCard(c);
    for (const c of tableCards) buf[p++] = wireStateCard(c);
    for (const c of namedCards) buf[p++] = wireStateCard(c);
    const n = ex.wasm_anim_stale_optimistic(optCards.length, tableCards.length, namedCards.length);
    if (n < 0) throw new Error(`anim_stale_optimistic error ${n}`);
    const out = mem(ex);
    const ob = ex.wasm_io_ptr();
    const rel: number[] = [];
    for (let i = 0; i < n; i++) rel.push(out[ob + i]);
    return rel;
}

/** One row of the end screen's finish order (anim_plan.h AnimFinishRow). */
export interface AnimFinishRow { place: number; seat: number; isYou: boolean }

/** THE FINISH ORDER, in C (anim_plan.h anim_finish_rows). Rank 1 is the first
 *  seat out, counting up to the fool last - whose place is the SEAT count, not
 *  the row count. `elimination` is first-out first; `gameOver` is the fool's
 *  seat, or negative while the game is still running; `mySeat` negative for a
 *  spectator, who owns no row. */
export function animFinishRows(
    elimination: number[], gameOver: number, nPlayers: number, mySeat: number,
): AnimFinishRow[] {
    const ex = bots();
    const buf = mem(ex);
    const base = ex.wasm_io_ptr();
    for (let i = 0; i < elimination.length; i++) buf[base + i] = elimination[i] & 0xff;
    const n = ex.wasm_anim_finish_rows(elimination.length, gameOver, nPlayers, mySeat);
    if (n < 0) throw new Error(`anim_finish_rows error ${n}`);
    const out = mem(ex);
    const ob = ex.wasm_io_ptr();
    const rows: AnimFinishRow[] = [];
    for (let i = 0; i < n; i++) {
        rows.push({ place: out[ob + i * 3], seat: out[ob + i * 3 + 1], isYou: out[ob + i * 3 + 2] !== 0 });
    }
    return rows;
}

// A slot holding a card the caller cannot NAME (anim_plan.h
// ANIM_TABLE_UNKNOWN): a replay's face-down hand card. Not an empty cell, which
// is ANIM_TABLE_NONE below.
const ANIM_TABLE_UNKNOWN = 0xff;

/**
 * THE ORDER A HAND IS DRAWN IN, in C (anim_plan.h anim_hand_laid_out_masked).
 *
 * `cards` is the hand as the kernel hands it over and `order` the viewer's
 * preferred arrangement, both of them plain cards with `null` for a slot the
 * caller cannot name. Cards the order no longer holds drop out, cards it never
 * knew append in kernel order, and a face-down slot is reconciled by COUNT,
 * because it has no identity to be stale about.
 *
 * ONE DOOR. The web had three of these - mergeReplayHandOrder by count,
 * mergeHandOrder by key set, reconcileHandMemory/displayedHand by key - so one
 * rearrangement scrubbed through a replay and played live could come out two
 * different ways. Asserted natively (c/tests/anim_plan_test.c
 * test_hand_order_with_hidden_slots).
 */
export function animHandLaidOut(
    cards: readonly (Card | null)[], order: readonly (Card | null)[], deferred = 0n,
): (Card | null)[] {
    if (cards.length > 36 || order.length > 36) throw new Error('anim: hand exceeds ABI cap');
    const ex = bots();
    const buf = mem(ex);
    const base = ex.wasm_io_ptr();
    let p = base;
    for (const c of cards) buf[p++] = c ? wireStateCard(c) : ANIM_TABLE_UNKNOWN;
    for (const c of order) buf[p++] = c ? wireStateCard(c) : ANIM_TABLE_UNKNOWN;
    const n = ex.wasm_anim_hand_laid_out(
        cards.length, order.length,
        Number(deferred & 0xffffffffn), Number((deferred >> 32n) & 0xffffffffn));
    if (n < 0) throw new Error(`anim_hand_laid_out error ${n}`);
    const out = mem(ex);
    const ob = ex.wasm_io_ptr();
    const laid: (Card | null)[] = [];
    for (let i = 0; i < n; i++) {
        const b = out[ob + i];
        laid.push(b === ANIM_TABLE_UNKNOWN ? null : cardFromWire(b));
    }
    return laid;
}

// legal.h/anim_plan.h spell "no card here" the same byte.
const ANIM_TABLE_NONE = 0xfe;

/** anim_plan.h ANIM_DEST_*: which kind of place a doomed motion put its card. */
export const ANIM_DEST = { table: 0, hand: 1, pool: 2 } as const;
/** anim_plan.h ANIM_CONFLICT_*, in the order the C defines them. The CODES are
 *  generated (sdk/ts/gen/anim.bots.ts); only the names are TypeScript's. */
export const ANIM_CONFLICT_REVERT = A.ANIM_CONFLICT_REVERT;
export const ANIM_CONFLICT_KEEP = A.ANIM_CONFLICT_KEEP;
export const ANIM_CONFLICT_CLEAR = A.ANIM_CONFLICT_CLEAR;
export const ANIM_CONFLICT = ['revert', 'keep', 'clear'] as const;
export type AnimConflictVerdict = (typeof ANIM_CONFLICT)[number];

/** One motion a superseded move made: which card, and what kind of place it
 *  landed in. `isCover` marks the defender's own play, which the capacity rule
 *  excludes (capacity is an attack rule). */
export interface AnimConflictMotion {
    card: Card | null;                     // null models a viewer-masked back
    dest: (typeof ANIM_DEST)[keyof typeof ANIM_DEST];
    isCover?: boolean;
}

/** One event of the arriving broadcast, as the conflict rule reads it. Which
 *  events SWEEP - and so whether the table was cleared - is the kernel's to
 *  decide (anim_conflict_sweep); this hands it the stream, not a verdict. */
export interface AnimConflictEvent {
    type: number;            // ANIM_EVT_* (animEventTypeCode)
    cards?: (Card | null)[];
    masked?: boolean;        // viewer-masked backs: they name nothing
}

/** What the arriving broadcast vouches for, and the server transport's extra
 *  question (anim_plan.h AnimServerHope). */
export interface AnimConflictInputs {
    /** The arriving stream's own events. The sweep is derived from these. */
    events: AnimConflictEvent[];
    /** The table of the board it opens on; both sides of a battle stand. */
    openTable: { attack: Card; defense: Card | null }[];
    /** My hand on that board. */
    myHand: Card[];
    pendingAttacks: number;
    defenderHand: number;
    finalUncovered: number;
}

/**
 * THE CONFLICT VERDICT per motion (anim_plan.h anim_conflict_facts +
 * anim_conflict_verdict), server transport. One door for all four shapes
 * AnimationContext asks about.
 *
 * The stream goes in as EVENTS: which of them sweep the table is part of the
 * rule (anim_conflict_sweep), not part of the marshal.
 */
export function animConflictVerdicts(
    motions: AnimConflictMotion[], inputs: AnimConflictInputs,
): AnimConflictVerdict[] {
    if (motions.length === 0) return [];
    const ex = bots();
    const buf = mem(ex);
    let p = ex.wasm_io_ptr();
    buf[p++] = inputs.events.length & 0xff;
    for (const e of inputs.events) {
        const cards = e.cards ?? [];
        buf[p++] = e.type & 0xff;
        buf[p++] = e.masked ? 1 : 0;
        buf[p++] = cards.length & 0xff;
        for (const c of cards) buf[p++] = c ? wireStateCard(c) : ANIM_TABLE_NONE;
    }
    buf[p++] = inputs.openTable.length & 0xff;
    for (const b of inputs.openTable) {
        buf[p++] = wireStateCard(b.attack);
        buf[p++] = b.defense ? wireStateCard(b.defense) : ANIM_TABLE_NONE;
    }
    buf[p++] = inputs.myHand.length & 0xff;
    for (const c of inputs.myHand) buf[p++] = wireStateCard(c);
    buf[p++] = motions.length & 0xff;
    for (const m of motions) {
        buf[p++] = m.card ? wireStateCard(m.card) : ANIM_TABLE_NONE;
        buf[p++] = m.dest;
        buf[p++] = m.isCover ? 1 : 0;
    }
    const n = ex.wasm_anim_conflict_verdicts(
        inputs.pendingAttacks, inputs.defenderHand, inputs.finalUncovered);
    if (n < 0) throw new Error(`anim_conflict_verdicts error ${n}`);
    const out = mem(ex);
    const ob = ex.wasm_io_ptr();
    const verdicts: AnimConflictVerdict[] = [];
    for (let i = 0; i < n; i++) verdicts.push(ANIM_CONFLICT[out[ob + i]]);
    return verdicts;
}

// ---------------------------------------------------------------------------
// The plan, the frame and the beats (anim_plan.h)
// ---------------------------------------------------------------------------
// WHAT A HOST WITH A FRAME LOOP ASKS. `animBuildPlan` turns a decoded viewer
// sequence into the kernel's timed plan - durations, start offsets, the
// count-freeze and the veil - and `animPlanAt` samples it at a clock the host
// owns, so a push landing mid-flight is answered by the next call instead of by
// editing the queue a timer chain is walking.
//
// The structs are read WHERE THEY LIE, through the generated accessors, and
// only the scalars a caller actually uses are copied out: a plan is 11,752
// bytes and a frame loop that marshalled it whole once per frame would spend
// more time copying than animating.

// The timing policy and the two frame sentinels, generated from anim_plan.h:
// ANIM_STEP_NONE ("no step is playing at this instant"), ANIM_NEVER ("the
// answer will not change again"), and the duration and gap every step is paced
// by. Re-exported here so a caller of this bridge needs one import, not two.
export const ANIM_STEP_NONE = A.ANIM_STEP_NONE;
export const ANIM_NEVER = A.ANIM_NEVER;
export const ANIM_TIME_MS = A.ANIM_TIME_MS;
export const ANIM_GAP_MS = A.ANIM_GAP_MS;

/** One decoded event as the plan sees it (anim_plan.h AnimPlanEvent). */
export interface AnimPlanEventIn {
    type: number;                   // ANIM_EVT_*
    seat?: number;                  // the acting seat; absent for none
    from?: number; to?: number;     // ANIM_LOC_*
    cards?: readonly Card[];
    maskCards?: boolean;            // viewer-masked backs: no identity, no veil
    /** THIS step's own board, when the wire carried one. */
    counts?: { deck: number; discard: number; flipped: Card | null; hand: readonly number[] };
    /** The row that board held, 2 bytes per battle (attack, then its cover or ANIM_TABLE_NONE). */
    battles?: readonly number[];
}

/** The count-freeze (anim_plan.h AnimCounts): the board the display holds until a step lands. */
export interface AnimCountsSnap {
    deck: number; discard: number; hand: number[]; nPlayers: number;
    nBattles: number; battles: number[]; paired: boolean; flipped: Card | null;
}

/** One planned step (anim_plan.h AnimPlanStep). */
export interface AnimPlanStepSnap {
    type: number; seat: number; from: number; to: number; nCards: number;
    durationMs: number; startMs: number;
    deck: number; discard: number; hand: number[];
    inFlightFromDeck: number; inFlightToFlipped: number;
    reveals: bigint;
}

/** The plan (anim_plan.h AnimPlan). */
export interface AnimPlanSnap {
    nSteps: number; totalMs: number; pre: AnimCountsSnap;
    veilIds: number[]; steps: AnimPlanStepSnap[];
}

/** Where the plan stands at a moment (anim_plan.h AnimFrame). */
export interface AnimFrameSnap {
    step: number; elapsedMs: number; landed: number; nextMs: number; done: boolean;
    deck: number; discard: number; hand: number[]; nPlayers: number; flipped: Card | null;
    inFlightFromDeck: number; inFlightToFlipped: number;
    veiled: bigint;
}

/** One event as the beat rules see it (anim_plan.h AnimBeatEvent). */
export interface AnimBeatEventIn {
    type: number; seat?: number; cards?: readonly Card[]; maskCards?: boolean;
    /** good_players_mask of THIS step's own board; absent for a step with none. */
    goodMask?: number;
}

/** One beat (anim_plan.h AnimBeat). */
export interface AnimBeatSnap {
    first: number; nEvents: number; type: number; seat: number; flags: number;
    outsMask: number; attackPassSeats: number; placedIds: bigint; goodMask: number;
}

/** The beats of a stream (anim_plan.h AnimBeats). */
export interface AnimBeatsSnap { beats: AnimBeatSnap[]; placedIds: bigint; firstGoodMask: number }

/** THE TIMED PLAN for a decoded viewer sequence (anim_plan.h anim_build_plan).
 *  `final` is the board the host already holds; the plan freezes the DISPLAY
 *  back to the pre-sequence values and reveals forward one step at a time. */
export function animBuildPlan(
    events: readonly AnimPlanEventIn[], nPlayers: number,
    final: { deck: number; discard: number; flipped: Card | null; hand: readonly number[] },
): AnimPlanSnap {
    const ex = bots();
    const buf = mem(ex);
    let p = ex.wasm_io_ptr();
    for (const e of events) {
        buf[p++] = e.type & 0xff;
        buf[p++] = e.seat === undefined || e.seat < 0 ? ANIM_W_NONE : e.seat & 0xff;
        buf[p++] = e.from === undefined ? ANIM_LOC_NONE : e.from & 0xff;
        buf[p++] = e.to === undefined ? ANIM_LOC_NONE : e.to & 0xff;
        buf[p++] = e.maskCards ? 1 : 0;
        const cards = e.cards ?? [];
        buf[p++] = cards.length & 0xff;
        for (const c of cards) buf[p++] = wireStateCard(c);
        buf[p++] = e.counts ? 1 : 0;
        if (e.counts) {
            buf[p++] = e.counts.deck & 0xff;
            buf[p++] = e.counts.discard & 0xff;
            buf[p++] = wireLogCard(e.counts.flipped);
            for (let s = 0; s < nPlayers; s++) buf[p++] = (e.counts.hand[s] ?? 0) & 0xff;
        }
        if (e.battles === undefined) { buf[p++] = ANIM_W_NONE; continue; }
        buf[p++] = (e.battles.length >> 1) & 0xff;
        for (const b of e.battles) buf[p++] = b & 0xff;
    }
    for (let s = 0; s < nPlayers; s++) buf[p++] = (final.hand[s] ?? 0) & 0xff;
    const rc = ex.wasm_anim_build_plan(events.length, nPlayers, final.deck, final.discard,
                                       wireLogCard(final.flipped));
    if (rc < 0) throw new Error(`anim_build_plan error ${rc}`);
    return readPlan(ex);
}

/** WHERE THE LAST-BUILT PLAN STANDS at `nowMs` from its start (anim_plan_at). */
export function animPlanAt(nowMs: number): AnimFrameSnap {
    const ex = bots();
    const rc = ex.wasm_anim_plan_at(Math.max(0, Math.round(nowMs)));
    if (rc < 0) throw new Error(`anim_plan_at error ${rc}`);
    const m = animMemOf(ex.memory.buffer);
    const at = ex.wasm_anim_frame_ptr();
    const nPlayers = A.AnimFrame_get_n_players(m, at);
    const hand: number[] = [];
    for (let s = 0; s < nPlayers; s++) hand.push(A.AnimFrame_get_hand(m, at, s));
    return {
        step: A.AnimFrame_get_step(m, at),
        elapsedMs: A.AnimFrame_get_elapsed_ms(m, at),
        landed: A.AnimFrame_get_landed(m, at),
        nextMs: A.AnimFrame_get_next_ms(m, at),
        done: A.AnimFrame_get_done(m, at) !== 0,
        deck: A.AnimFrame_get_deck(m, at),
        discard: A.AnimFrame_get_discard(m, at),
        hand, nPlayers,
        flipped: readAnimCard(m, A.AnimFrame_flipped_at(at)),
        inFlightFromDeck: A.AnimFrame_get_in_flight_from_deck(m, at),
        inFlightToFlipped: A.AnimFrame_get_in_flight_to_flipped(m, at),
        veiled: A.AnimFrame_get_veiled(m, at),
    };
}

/** THE BEATS a stream plays in (anim_plan.h anim_build_beats). */
export function animBuildBeats(events: readonly AnimBeatEventIn[]): AnimBeatsSnap {
    const ex = bots();
    const buf = mem(ex);
    let p = ex.wasm_io_ptr();
    for (const e of events) {
        buf[p++] = e.type & 0xff;
        buf[p++] = e.seat === undefined || e.seat < 0 ? ANIM_W_NONE : e.seat & 0xff;
        buf[p++] = e.maskCards ? 1 : 0;
        buf[p++] = e.goodMask === undefined ? 0 : 1;
        buf[p++] = (e.goodMask ?? 0) & 0xff;
        const cards = e.cards ?? [];
        buf[p++] = cards.length & 0xff;
        for (const c of cards) buf[p++] = wireStateCard(c);
    }
    const n = ex.wasm_anim_build_beats(events.length);
    if (n < 0) throw new Error(`anim_build_beats error ${n}`);
    const m = animMemOf(ex.memory.buffer);
    const at = ex.wasm_anim_beats_ptr();
    const beats: AnimBeatSnap[] = [];
    for (let i = 0; i < n; i++) {
        const b = A.AnimBeats_beats_at(at, i);
        beats.push({
            first: A.AnimBeat_get_first(m, b),
            nEvents: A.AnimBeat_get_n_events(m, b),
            type: A.AnimBeat_get_type(m, b),
            seat: A.AnimBeat_get_seat(m, b),
            flags: A.AnimBeat_get_flags(m, b),
            outsMask: A.AnimBeat_get_outs_mask(m, b),
            attackPassSeats: A.AnimBeat_get_attack_pass_seats(m, b),
            placedIds: A.AnimBeat_get_placed_ids(m, b),
            goodMask: A.AnimBeat_get_good_mask(m, b),
        });
    }
    return {
        beats,
        placedIds: A.AnimBeats_get_placed_ids(m, at),
        firstGoodMask: A.AnimBeats_get_first_good_mask(m, at),
    };
}

// "no seat" on the wire in, mirroring wasm_api.c's ANIM_W_NONE.
const ANIM_W_NONE = 0xff;
const ANIM_LOC_NONE = A.ANIM_LOC_NONE;

// A Card inside a kernel struct is a packed byte, not the wire's dense id.
// CARD_NONE is what the kernel writes for "no flipped trump left".
function readAnimCard(m: ReturnType<typeof animMemOf>, at: number): Card | null {
    const suit = A.Card_get_suit(m, at), value = A.Card_get_value(m, at);
    return suit < 0 || value < 1 ? null : { suit, value };
}

function readPlan(ex: BotsExports): AnimPlanSnap {
    const m = animMemOf(ex.memory.buffer);
    const at = ex.wasm_anim_plan_ptr();
    const nSteps = A.AnimPlan_get_n_steps(m, at);
    const preAt = A.AnimPlan_pre_at(at);
    const nPlayers = A.AnimCounts_get_n_players(m, preAt);
    const preHand: number[] = [];
    for (let s = 0; s < nPlayers; s++) preHand.push(A.AnimCounts_get_hand(m, preAt, s));
    const nBattles = A.AnimCounts_get_n_battles(m, preAt);
    const battles: number[] = [];
    for (let i = 0; i < 2 * nBattles; i++) battles.push(A.AnimCounts_get_battles(m, preAt, i));
    const steps: AnimPlanStepSnap[] = [];
    for (let i = 0; i < nSteps; i++) {
        const s = A.AnimPlan_steps_at(at, i);
        const hand: number[] = [];
        for (let k = 0; k < nPlayers; k++) hand.push(A.AnimPlanStep_get_hand(m, s, k));
        steps.push({
            type: A.AnimPlanStep_get_type(m, s),
            seat: A.AnimPlanStep_get_seat(m, s),
            from: A.AnimPlanStep_get_from(m, s),
            to: A.AnimPlanStep_get_to(m, s),
            nCards: A.AnimPlanStep_get_n_cards(m, s),
            durationMs: A.AnimPlanStep_get_duration_ms(m, s),
            startMs: A.AnimPlanStep_get_start_ms(m, s),
            deck: A.AnimPlanStep_get_deck(m, s),
            discard: A.AnimPlanStep_get_discard(m, s),
            hand,
            inFlightFromDeck: A.AnimPlanStep_get_in_flight_from_deck(m, s),
            inFlightToFlipped: A.AnimPlanStep_get_in_flight_to_flipped(m, s),
            reveals: A.AnimPlanStep_get_reveals(m, s),
        });
    }
    const nVeil = A.AnimPlan_get_n_veil(m, at);
    const veilIds: number[] = [];
    for (let i = 0; i < nVeil; i++) veilIds.push(A.AnimPlan_get_veil_ids(m, at, i));
    return {
        nSteps, totalMs: A.AnimPlan_get_total_ms(m, at), steps, veilIds,
        pre: {
            deck: A.AnimCounts_get_deck(m, preAt), discard: A.AnimCounts_get_discard(m, preAt),
            hand: preHand, nPlayers, nBattles, battles,
            paired: A.AnimCounts_get_paired(m, preAt) !== 0,
            flipped: readAnimCard(m, A.AnimCounts_flipped_at(preAt)),
        },
    };
}


/**
 * THE ORDER A DOOMED SEQUENCE FLIES HOME IN (anim_plan.h anim_reversal_order),
 * for a caller that already holds its verdicts - which every server-transport
 * caller does, since anim_conflict_verdict is the only entry that asks the
 * AnimServerHope.
 *
 * `verdicts` are ANIM_CONFLICT_* per motion in the order the motions flew, and
 * `groupSizes` slices them into the parallel steps they flew as. The answer is
 * the steps to play, each a list of motion indices, in REVERSE group order: the
 * cards travel back the way they came, last group first, and a group nothing
 * reverts is dropped rather than played as a beat of silence.
 */
export function animReversalOrder(
    verdicts: readonly number[], groupSizes: readonly number[],
): number[][] {
    const ex = bots();
    const buf = mem(ex);
    const base = ex.wasm_io_ptr();
    let p = base;
    buf[p++] = verdicts.length & 0xff;
    for (const v of verdicts) buf[p++] = v & 0xff;
    buf[p++] = groupSizes.length & 0xff;
    for (const g of groupSizes) buf[p++] = g & 0xff;
    const n = ex.wasm_anim_reversal_order();
    if (n < 0) throw new Error(`anim_reversal_order error ${n}`);
    const out = mem(ex);
    const ob = ex.wasm_io_ptr();
    const counts: number[] = [];
    for (let i = 0; i < n; i++) counts.push(out[ob + i]);
    const steps: number[][] = [];
    let at = ob + n;
    for (const c of counts) {
        const step: number[] = [];
        for (let i = 0; i < c; i++) step.push(out[at++]);
        steps.push(step);
    }
    return steps;
}
