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
import { memOf as viewMemOf, readTableView, readReplaySummary, TableView_Snap, ReplaySummary_Snap, CARD_NONE_SUIT, CARD_NONE_VALUE } from '../gen/view_layout.bots.ts';

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
    wasm_export_moves(start: number, max: number): number;
    wasm_replay_io_ptr(): number;
    wasm_replay_io_cap(): number;
    wasm_replay_error_detail(): number;
    wasm_msg_decode?(len: number): number;
    wasm_msg_seal?(len: number): number;
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

// Worst-case wire bytes for one exported move: type + n + 2 x MAX_MOVE_CARDS
// wire cards (the wasm build pins MAX_MOVE_CARDS=28). The loop advances by the
// RETURNED count, so a mismatched clamp can shorten chunks but never skip moves.
const MOVE_WIRE_MAX = 2 + 2 * 28;

// The legal moves of whatever game is RESIDENT in the kernel, no marshal.
function residentLegalMoves(ex: EngineExports, seat: number): { type: string; cards?: Card[]; attack_cards?: Card[] }[] {
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

// The session log record types (c/src/game.h LOG_*), for the desync message only.
const LOG_TYPE_NAMES = ['game_start', 'attack', 'cover', 'pass', 'pickup', 'good', 'discard', 'defender_change', 'player_out', 'draw'];

// Mirrors REPLAY_E* in replay.h. The NUMBERS are the wire between the two
// files, so 3, 8 and 9 are a deliberate hole: they belonged to the retired
// retrodiction format and are unreachable now.
export function __replayError(negCode: number, detail: number): Error {
    switch (-negCode) {
        case 1: return new Error(`unsupported replay format version ${detail}`);
        case 2: return new Error('invalid replay: leftover data after game end');
        case 4: return new Error('replay guard: too many events');
        case 5: return new Error('replay desync: no legal moves');
        case 6: return new Error('replay desync: conservation');
        case 7: return new Error('replay desync: played card not in hand');
        case 10: return new Error('replay desync: the unseen pool is empty');
        case 11: return new Error('replay desync: a supplied reveal is not unseen');
        case 12: return new Error(
            `replay desync: logged ${LOG_TYPE_NAMES[detail >> 16] ?? (detail >> 16)} not in menu of ${detail & 0xffff}`);
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
    wasm_replay_events_n(): number;
    wasm_replay_events_next(): number;
    wasm_replay_step_count(code_len: number): number;
    wasm_replay_step_index(code_len: number): number;
    wasm_replay_step_masked_state(code_len: number, step: number, viewer: number): number;
    wasm_replay_step_logs(code_len: number, step: number): number;
    wasm_replay_summary(code_len: number): number;
    wasm_replay_extras_encode(in_len: number): number;
    wasm_replay_extras_decode(blob_len: number, player_count: number, move_count: number): number;
    wasm_replay_link(in_len: number, style: number): number;
    wasm_replay_b32_encode(in_len: number): number;
    wasm_replay_b32_decode(in_len: number): number;
    wasm_replay_link_parse(in_len: number): number;
    wasm_unambiguous_cover(n_cover: number, n_battles: number, power_suit: number): number;
    wasm_bot_roster_dump(): number;
    // Belief probe (observability; off until reset arms it)
    wasm_belief_probe_reset(): void;
    wasm_belief_probe_dump(): number;
    // Animation core (c/src/anim_plan.h) — the platform-independent animation
    // policy the web pure modules (src/state/*) delegate to. bots-only.
    wasm_anim_should_drop_stale(hasLast: number, last: number, hasIncoming: number, incoming: number): number;
    wasm_anim_stale_optimistic(nOpt: number, nTable: number, nNamed: number): number;
    wasm_anim_finish_rows(nElim: number, gameOver: number, nPlayers: number, mySeat: number): number;
    wasm_anim_conflict_verdicts(pendingAttacks: number, defenderHand: number,
                                finalUncovered: number): number;
    wasm_anim_set_transport(transport: number): number;
    wasm_anim_transport(): number;
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
    return parseBeliefProbe(new Uint8Array(ex.memory.buffer, base, n * 11), n);
}

/** The records wasm_belief_probe_dump wrote (11 bytes each), from any bots.wasm instance. */
export function parseBeliefProbe(buf: Uint8Array, n: number): BeliefProbeRecord[] {
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

// The unpacked header — the private ABI msg_blob_write/msg_blob_read define in
// c/wasm/wasm_api.c. Fixed offsets, fixed-size join slots.
// Round 16 added the two send-clock bytes at 90, the bubble delta at 92, and
// the fool's-penalty trio (opening at 93, carry_key at 94, carry_fool at 98),
// so the joins start at 99.
const MSG_BLOB_HDR = 99;
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
        opening: b[93],
        carry_key: dv.getUint32(94, true),
        carry_fool: b[98],
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
    // 93..99 IS written back: unlike the digest and the delta, the fool's
    // penalty is a term of the deal the caller states.
    out[93] = e.opening;
    dv.setUint32(94, e.carry_key >>> 0, true);
    out[98] = e.carry_fool;
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
    mem(ex).set(envelope, base);
    const r = ex.wasm_msg_decode(envelope.length);
    if (r < 0) throw msgError(r);
    return readBlob(mem(ex), base, r);
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

// A battle's cover as a wire byte. An uncovered battle's defense is no card: null
// from a caller that says so, the kernel's CARD_NONE on a board the client holds.
const coverByte = (d: Card | null): number =>
    !d || (d.suit === CARD_NONE_SUIT && d.value === CARD_NONE_VALUE) ? WIRE_NONE : wireLogCard(d);

/** The paired result: cover card i defends attackCards[i]. Mirrors the shape the
 * deleted coverCombinations.ts findUnambiguousCover returned. */
export interface CoverCombination { coverCards: Card[]; attackCards: Card[]; }

/**
 * If `coverCards` cover the table's uncovered attacks in exactly one unambiguous
 * way (every valid full pairing covers the same set of attacks), return that
 * pairing; otherwise null (the UI then lets the player place cards manually).
 */
export function kernelUnambiguousCover(
    coverCards: readonly Card[], tableBattles: readonly { attack: Card; defense: Card | null }[], powerSuit: number,
): CoverCombination | null {
    if (coverCards.length === 0) return null;
    const ex = bots();
    const buf = mem(ex);
    const aptr = ex.wasm_cards_a_ptr();
    for (let i = 0; i < coverCards.length; i++) buf[aptr + i] = wireLogCard(coverCards[i]);
    const bptr = ex.wasm_cards_b_ptr();
    for (let i = 0; i < tableBattles.length; i++) {
        buf[bptr + 2 * i] = wireLogCard(tableBattles[i].attack);
        buf[bptr + 2 * i + 1] = coverByte(tableBattles[i].defense);
    }
    const n = ex.wasm_unambiguous_cover(coverCards.length, tableBattles.length, powerSuit);
    if (n <= 0) return null;
    // Re-fetch the memory view: a wasm call can grow (and detach) the buffer.
    const out = mem(ex);
    const io = ex.wasm_io_ptr();
    const attackCards: Card[] = [];
    for (let i = 0; i < n; i++) attackCards.push(cardFromWire(out[io + i]));
    return { coverCards: [...coverCards], attackCards };
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

/** REPLAY_EXTRAS_FLAG_* (c/src/replay_extras.h). */
const EXTRAS_FLAG_NAMES = 1, EXTRAS_FLAG_TIMES = 2;

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

/** Pack the kernel's argument blob: flags, then the roster, then the timing. */
function __extrasArgs(names: string[] | null, startTime: number | null,
                      gaps: number[] | null): Uint8Array {
    const enc = new TextEncoder();
    const encoded = names ? names.map(n => enc.encode(n)) : [];
    const hasNames = encoded.length > 0;
    const hasTimes = startTime !== null && gaps !== null;
    let n = 1;
    if (hasNames) { n += 1; for (const b of encoded) n += 2 + b.length; }
    if (hasTimes) n += 8 + 2 + 8 * gaps!.length;
    const out = new Uint8Array(n);
    const dv = new DataView(out.buffer);
    let q = 0;
    out[q++] = (hasNames ? EXTRAS_FLAG_NAMES : 0) | (hasTimes ? EXTRAS_FLAG_TIMES : 0);
    if (hasNames) {
        out[q++] = encoded.length;
        for (const b of encoded) { dv.setUint16(q, b.length, true); q += 2; out.set(b, q); q += b.length; }
    }
    if (hasTimes) {
        dv.setFloat64(q, startTime!, true); q += 8;
        dv.setUint16(q, gaps!.length, true); q += 2;
        for (const g of gaps!) { dv.setFloat64(q, g, true); q += 8; }
    }
    return out;
}

/**
 * Roster + timing -> the raw extras blob. `names` must be as wide as the table
 * (a reader takes the seat count from the decoded moves); unnamed seats are ''.
 * Passing null for either half leaves that section out.
 */
export function kernelReplayExtrasEncode(names: string[] | null, startTime: number | null,
                                         gaps: number[] | null): Uint8Array {
    const ex = bots();
    const args = __extrasArgs(names, startTime, gaps);
    if (args.length > ex.wasm_replay_io_cap()) throw new Error('extras: roster exceeds the kernel IO buffer');
    mem(ex).set(args, ex.wasm_replay_io_ptr());
    const n = ex.wasm_replay_extras_encode(args.length);
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
    const n = ex.wasm_replay_extras_decode(blob.length, playerCount, moveCount);
    if (n < 0) throw __extrasError(n);
    const base = ex.wasm_io_ptr();
    const out = mem(ex).slice(base, base + n);
    const dv = new DataView(out.buffer, out.byteOffset, out.byteLength);
    const dec = new TextDecoder();
    let q = 0;
    const flags = out[q++];
    const nNames = out[q++];
    let namesOut: string[] | null = null;
    if (flags & EXTRAS_FLAG_NAMES) {
        namesOut = [];
        for (let i = 0; i < nNames; i++) {
            const len = dv.getUint16(q, true); q += 2;
            namesOut.push(dec.decode(out.subarray(q, q + len))); q += len;
        }
    }
    let startTime: number | null = null;
    let moveGaps: number[] | null = null;
    if (flags & EXTRAS_FLAG_TIMES) {
        startTime = dv.getFloat64(q, true); q += 8;
        const nGaps = dv.getUint16(q, true); q += 2;
        moveGaps = [];
        for (let i = 0; i < nGaps; i++) { moveGaps.push(dv.getFloat64(q, true)); q += 8; }
    }
    return { names: namesOut, startTime, moveGaps };
}

/**
 * THE WHOLE SHAREABLE LINK for a finished game: `https://foolish.cards/<moves>`
 * plus `-<base32 extras>` when the roster says anything, and the bare link when
 * nobody at the table is named. Built in the kernel rather than concatenated
 * here (c/src/replay_extras.h) - the prefix is a constant and the dash is the
 * codec's, so a phone, a watch and a browser have no business each writing it.
 * `names` must be as wide as the table; unnamed seats are ''.
 */
/** replay_extras.h REPLAY_LINK_STYLE_*: the https link, or the QR form. */
export const REPLAY_LINK = { url: 0, qr: 1 } as const;

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
    const enc = new TextEncoder();
    const movesBytes = enc.encode(moves);
    const encoded = names.map(n => enc.encode(n));
    let rosterLen = 0;
    for (const b of encoded) rosterLen += 2 + b.length;
    // [u8 n_names][u16 roster_len][roster][moves] - the code last, so the kernel
    // can NUL-terminate it in place instead of copying it out.
    const args = new Uint8Array(3 + rosterLen + movesBytes.length);
    const dv = new DataView(args.buffer);
    let q = 0;
    args[q++] = encoded.length;
    dv.setUint16(q, rosterLen, true); q += 2;
    for (const b of encoded) { dv.setUint16(q, b.length, true); q += 2; args.set(b, q); q += b.length; }
    args.set(movesBytes, q);
    // The kernel writes a terminator one byte past the input, so the arguments
    // must fit the buffer with room for it.
    if (args.length >= ex.wasm_replay_io_cap()) throw new Error('extras: link arguments exceed the kernel IO buffer');
    mem(ex).set(args, ex.wasm_replay_io_ptr());
    const w = ex.wasm_replay_link(args.length, style);
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
    const blob = writeBlob({ ...header, sent_at: header.sent_at ?? 0, n_new: 0,
                             opening: header.opening ?? MSG_NO_OPENING,
                             carry_key: header.carry_key ?? 0,
                             carry_fool: header.carry_fool ?? MSG_NO_FOOL,
                             format: 2, turn: 0, round: 0, digest: new Uint8Array(32) });
    const base = ex.wasm_replay_io_ptr();
    mem(ex).set(blob, base);
    const r = ex.wasm_msg_seal(blob.length);
    if (r < 0) throw msgError(r);
    return mem(ex).slice(base, base + r);
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
    if (n < 0) throw __replayError(n, ex.wasm_replay_error_detail());
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
    mem(ex).set(code, ex.wasm_replay_io_ptr());
    const len = ex.wasm_replay_step_index(code.length);
    if (len < 0) throw __replayError(len, ex.wasm_replay_error_detail());
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
    // no-progress backstop, not a step limit.
    for (let guard = 0; from < steps && guard < 4096; guard++) {
        mem(ex).set(code, ex.wasm_replay_io_ptr());
        const len = ex.wasm_replay_events(viewer, from, code.length);
        if (len < 0) throw __replayError(len, ex.wasm_replay_error_detail());
        const next = ex.wasm_replay_events_next();
        if (next <= from) throw new Error(`replay frames stalled at step ${from}/${steps}`);
        const buf = mem(ex);
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

// ANIM_EVT_* — mirrors anim_plan.h (which mirrors ANIMATION_EVENT_TYPE / EVW_T_*).
export const ANIM_EVT: Record<string, number> = {
    magic_transition: 0, deal: 1, flipped: 2, defender_move: 3, attack_pass: 4,
    cover: 5, pickup: 6, discard: 7, out: 8, refill: 9, cards_to_trash: 10, revert: 11,
};
// ANIM_LOC_* — mirrors anim_plan.h.
export const ANIM_LOC: Record<string, number> = {
    deck: 0, hand: 1, table: 2, discard: 3, flipped: 4,
};

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

// legal.h/anim_plan.h spell "no card here" the same byte.
const ANIM_TABLE_NONE = 0xfe;

/** anim_plan.h ANIM_DEST_*: which kind of place a doomed motion put its card. */
export const ANIM_DEST = { table: 0, hand: 1, pool: 2 } as const;
/** anim_plan.h ANIM_CONFLICT_*, in the order the C defines them. */
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

