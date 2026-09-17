// table_io.ts - a games row through the C Table (docs/C_GAME_SHAPE_MIGRATION.md
// 2.3-2.5, Phase 4b).
//
// Every server operation on a game is the same shape, and this module is that
// shape: load the row, run ONE synchronous kernel section (load the table, do
// the operation, copy every product out), commit the products with the
// version-fenced commit_table RPC, finish a game that just ended, and broadcast
// the kernel's per-viewer pushes. What is here is I/O only: which columns to
// read and write, the RPC and its parameter names, the realtime topics, hex and
// base64 as the transports need them, the retry loop and the clock. What a
// state blob, a roster, a seat, a status or a push means is the kernel's
// (sdk/ts/table/server_table.ts over c/src/table.h): nothing here reads inside
// one.
//
// CONCURRENCY. We can't hold a real DB lock across load -> compute -> save:
// PostgREST gives one transaction per call. So the row is loaded WITH its
// version, the operation runs, and commit_table writes only if games.version is
// still what was loaded, then bumps it. The commit is one transaction across
// games, the membership rows and the view rows (no torn reads), and a stale or
// slow execution can never overwrite a newer state. On a conflict the row is
// reloaded and the operation runs again.
//
// ONE KERNEL SECTION. The table is the module's resident game, shared by every
// request this isolate serves. Nothing read from it survives an await: each
// attempt loads, operates and copies its products out before its first await.

import * as L from '@sdk/ts/gen/game_layout.bots.ts';
import {
    gameStatusLabel, serverTable, tableCodeName, TABLE_DEAL_SEED_BYTES,
    type ServerTable, type TableProducts, type TableSeat,
} from '@sdk/ts/table/server_table.ts';
import { bytesToBase64 } from '@sdk/ts/wire/bytes.ts';
import { broadcastMessages, supabaseClient, type BroadcastMessage } from './utils.ts';
import { getCachedRow, invalidateCachedRow, noteCommittedRow } from './game_cache.ts';

// A lazy import that resolves ONCE: the end of a game is rare, and its module
// graph (the replay tail) stays off every other request's cold start.
const lazy = <T>(load: () => Promise<T>): (() => Promise<T>) => {
    let mod: Promise<T> | undefined;
    return () => (mod ??= load());
};
const finalizeMod = lazy(() => import('./finalize.ts'));

// ---- column transports ----------------------------------------------------

// Every commit writes several kilobytes of hex, so the transport writes the
// character codes into one buffer and decodes it once: no string per byte, no
// garbage for the collector to chase between moves.
const HEX_CODES = new Uint8Array(512);
for (let b = 0; b < 256; b++) {
    HEX_CODES[2 * b] = '0123456789abcdef'.charCodeAt(b >> 4);
    HEX_CODES[2 * b + 1] = '0123456789abcdef'.charCodeAt(b & 15);
}
const ascii = new TextDecoder();
const HEX_NIBBLE = new Int8Array(128).fill(-1);
for (let i = 0; i < 16; i++) {
    HEX_NIBBLE['0123456789abcdef'.charCodeAt(i)] = i;
    HEX_NIBBLE['0123456789ABCDEF'.charCodeAt(i)] = i;
}

/** Bare hex (no '\x'), the text form of the log, view and spectator columns. */
export function bareHex(b: Uint8Array): string {
    const out = new Uint8Array(2 * b.length);
    for (let i = 0; i < b.length; i++) {
        const v = 2 * b[i];
        out[2 * i] = HEX_CODES[v];
        out[2 * i + 1] = HEX_CODES[v + 1];
    }
    return ascii.decode(out);
}

/** '\x'-prefixed hex, the text form of the state and roster columns. */
export const bytesToColumnHex = (b: Uint8Array): string => `\\x${bareHex(b)}`;

/** Bytes of a hex column, with or without the '\x' prefix. */
export function columnHexToBytes(hex: string): Uint8Array {
    const at = hex.startsWith('\\x') ? 2 : 0;
    const out = new Uint8Array((hex.length - at) >> 1);
    for (let i = 0; i < out.length; i++) {
        const hi = HEX_NIBBLE[hex.charCodeAt(at + 2 * i)], lo = HEX_NIBBLE[hex.charCodeAt(at + 2 * i + 1)];
        if (hi < 0 || lo < 0) throw new RangeError('table_io: a hex column holds a non-hex character');
        out[i] = (hi << 4) | lo;
    }
    return out;
}

// ---- the row ----------------------------------------------------------------

/** The columns a table operation reads. */
export interface TableRow {
    gameId: string;
    version: number;
    roundEpoch: number;
    state: Uint8Array;
    roster: Uint8Array;
    /** The roster column's text as stored: written back as is when an operation leaves the roster alone. */
    rosterHex: string;
    gameSeed: string | null;
    /** The row came from this isolate's cache, not a fresh SELECT. */
    cached: boolean;
}

/** A game id that names no row. */
export class GameNotFound extends Error {
    constructor(gameId: string) { super(`Game ${gameId} not found`); this.name = 'GameNotFound'; }
}

/** Loads a row: this isolate's cache when allowed and warm, else a SELECT. */
export async function loadRow(gameId: string, allowCache = true): Promise<TableRow> {
    const hit = allowCache ? getCachedRow(gameId) : undefined;
    if (hit) {
        return {
            gameId, version: hit.version, roundEpoch: hit.roundEpoch,
            state: hit.state, roster: hit.roster, rosterHex: hit.rosterHex,
            gameSeed: hit.gameSeed, cached: true,
        };
    }
    // Only what an operation reads: logs_packed in particular grows all session
    // and never rides along.
    const { data, error } = await supabaseClient
        .from('games')
        .select('id, version, round_epoch, state, roster, game_seed')
        .eq('id', gameId).single();
    if (error || !data) throw new GameNotFound(gameId);
    if (!data.state || !data.roster) throw new Error(`Game ${gameId} has no state or roster blob`);
    return {
        gameId,
        // A BIGINT column can arrive as a string; `"1" + 1` is "11".
        version: Number(data.version ?? 0),
        roundEpoch: Number(data.round_epoch ?? 0),
        state: columnHexToBytes(data.state),
        roster: columnHexToBytes(data.roster),
        rosterHex: data.roster,
        gameSeed: data.game_seed ?? null,
        cached: false,
    };
}

// ---- refusals ----------------------------------------------------------------

/** The kernel refused an operation (a negative table result). */
export class TableRefusal extends Error {
    constructor(readonly code: number, readonly detail: number, message: string) {
        super(message);
        this.name = 'TableRefusal';
    }
}

/** The HTTP-facing sentence for a refusal. The judgement is the kernel's; this only words it. */
export function refusalMessage(code: number, detail: number, gameId: string, actorId: string): string {
    switch (code) {
        case L.TABLE_E_NOT_SEATED: return `Player ${actorId} not in game ${gameId}`;
        case L.TABLE_E_NOT_WAITING: return `Game ${gameId} is not in its lobby`;
        case L.TABLE_E_NOT_OVER: return `Game ${gameId} is not over`;
        case L.TABLE_E_FORBIDDEN: return `Not allowed in game ${gameId}`;
        case L.TABLE_E_WIRE: return `Malformed request for game ${gameId}`;
        case L.TABLE_E_UNKNOWN_BRAIN: return `Game ${gameId} seats a bot this server cannot play`;
        case L.TABLE_E_ROSTER:
            switch (detail) {
                case L.ROSTER_E_FULL: return `Game is full (max ${L.Roster_seats_LEN} players)`;
                case L.ROSTER_E_DUPLICATE: return `Player is already in game ${gameId}`;
                case L.ROSTER_E_TITLE: return 'The game name must be 1-50 characters';
                case L.ROSTER_E_PERM: return 'The new order must name every seated player exactly once';
                default: return `Roster refused for game ${gameId}: ${tableCodeName(detail, ['ROSTER_E_'])}`;
            }
        default:
            return `Game ${gameId} refused: ${tableCodeName(code, ['TABLE_E_', 'GAME_INVALID_'])}`;
    }
}

// ---- the deal seed ---------------------------------------------------------------

let dealSeedOverride: Uint8Array | null = null;

/** Test hook: every deal uses `seed` (32 bytes) instead of crypto; null restores crypto. */
export function __setTableDealSeedOverride(seed: Uint8Array | null): void {
    dealSeedOverride = seed ? seed.slice() : null;
}

/** The bytes a lobby edit that may deal hands the kernel. */
export function drawDealSeed(): Uint8Array {
    if (dealSeedOverride) return dealSeedOverride.slice();
    const seed = new Uint8Array(TABLE_DEAL_SEED_BYTES);
    crypto.getRandomValues(seed);
    return seed;
}

// ---- one operation ---------------------------------------------------------

/** What an operation's kernel section is handed. */
export interface TableSection {
    table: ServerTable;
    row: TableRow;
    /** 32 fresh bytes per attempt, for an edit that may deal. */
    dealSeed: Uint8Array;
}

export interface TableOp {
    gameId: string;
    reqId: string;
    /** The caller's auth id: whose envelope the outcome carries (the spectator's when not seated). */
    viewerId: string | null;
    /** SYNCHRONOUS: the operation on the loaded table. Returns a table result code. */
    run(s: TableSection): number;
    /** Results that commit nothing (the default: none). */
    noCommit?: (rc: number) => boolean;
    /** Results only authoritative against a fresh row: from a cached row, reload and run again. */
    freshOnly?: (rc: number) => boolean;
    /** An operation that never reads this isolate's cache (the default reads it). */
    fresh?: boolean;
}

export interface TableOutcome {
    /** The table result code (never negative: refusals throw TableRefusal). */
    result: number;
    /** ENGINE_REJECT_* behind a TABLE_REJECTED. */
    reject: number;
    /** The version the row now has: the committed one, or the loaded one when nothing committed. */
    version: number;
    /** The row was deleted (the last seat left). */
    deleted: boolean;
    /** Something committed. */
    committed: boolean;
    /** The committed row has a bot to drive. */
    needsBots: boolean;
    /** The caller's response envelope at `version`. */
    envelope: Uint8Array;
}

const MAX_ATTEMPTS = 5;

/** Everything one attempt copied out of the kernel. */
interface Copied {
    rc: number;
    reject: number;
    detail: number;
    envelope: Uint8Array | null;
    products: TableProducts | null;
    seats: TableSeat[];
    pushes: { viewer: number; bytes: Uint8Array }[];
    needsBots: boolean;
}

/** The CAS loop around one table operation. */
export async function runTableOp(op: TableOp): Promise<TableOutcome> {
    const table = await serverTable();
    for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        const row = await loadRow(op.gameId, !op.fresh && attempt === 1);
        const dealSeed = drawDealSeed();
        const nextVersion = row.version + 1;

        // ---- the kernel section: nothing below awaits until it ends ----
        const c = section(table, row, op, dealSeed, nextVersion);
        // ---- end of the kernel section ----

        if (c.rc < 0) {
            if (row.cached) { invalidateCachedRow(op.gameId); continue; }
            throw new TableRefusal(c.rc, c.detail, refusalMessage(c.rc, c.detail, op.gameId, op.viewerId ?? ''));
        }
        if (row.cached && op.freshOnly?.(c.rc)) { invalidateCachedRow(op.gameId); continue; }

        if (c.rc === L.TABLE_EMPTY) {
            // The last seat left: the row goes, fenced on the version like a commit.
            const { data, error } = await supabaseClient
                .from('games').delete().eq('id', op.gameId).eq('version', row.version).select('id');
            if (error) throw error;
            invalidateCachedRow(op.gameId);
            if (!data || data.length === 0) continue;   // somebody committed first
            return { result: c.rc, reject: 0, version: row.version, deleted: true, committed: true, needsBots: false, envelope: c.envelope! };
        }

        if (!c.products) {
            return { result: c.rc, reject: c.reject, version: row.version, deleted: false, committed: false, needsBots: c.needsBots, envelope: c.envelope! };
        }

        const p = c.products;
        const version = await commitProducts(op.gameId, row, p, c.seats, p.dealtNow ? dealSeed : null);
        if (version === null) {
            invalidateCachedRow(op.gameId);
            continue;
        }

        if (p.ended) {
            const { finalizeEndedGame } = await finalizeMod();
            await finalizeEndedGame(op.gameId, p.state, p.roster, op.reqId);
        }
        if (c.pushes.length > 0) {
            broadcastPushes(op.gameId, version, c.seats, c.pushes, op.reqId)
                .catch((err) => console.error(`[${op.reqId}] broadcast failed:`, err));
        }
        return { result: c.rc, reject: c.reject, version, deleted: false, committed: true, needsBots: p.needsBots, envelope: c.envelope! };
    }
    throw new Error(`Could not commit game ${op.gameId} after ${MAX_ATTEMPTS} attempts - write contention`);
}

function section(table: ServerTable, row: TableRow, op: TableOp, dealSeed: Uint8Array, nextVersion: number): Copied {
    const none = { reject: 0, detail: 0, envelope: null, products: null, seats: [], pushes: [], needsBots: false };
    const loaded = table.load(row.state, row.roster);
    if (loaded < 0) {
        // A stored row the kernel refuses: never silently repaired or played on.
        throw new Error(`Game ${op.gameId} does not load: ${tableCodeName(loaded, ['TABLE_E_', 'GAME_INVALID_'])} (detail ${table.detail()})`);
    }
    const rc = op.run({ table, row, dealSeed });
    if (rc < 0) return { ...none, rc, detail: table.detail() };
    const reject = rc === L.TABLE_REJECTED ? table.reject() : 0;

    if (rc === L.TABLE_EMPTY) {
        return { ...none, rc, envelope: envelopeOrThrow(table.envelope(op.gameId, -1, row.version)) };
    }
    if (op.noCommit?.(rc)) {
        const seat = op.viewerId !== null ? table.seatOf(op.viewerId) : -1;
        return {
            ...none, rc, reject, needsBots: table.needsBots(),
            envelope: envelopeOrThrow(table.envelope(op.gameId, seat, row.version)),
        };
    }

    const products = table.commit(op.gameId, nextVersion, Date.now());
    if (typeof products === 'number') {
        throw new Error(`Game ${op.gameId}: no commit products: ${tableCodeName(products, ['TABLE_E_'])}`);
    }
    const seats = table.seats();
    const pushes: { viewer: number; bytes: Uint8Array }[] = [];
    if (products.nEvents > 0) {
        for (let s = 0; s < seats.length; s++) {
            if (seats[s].brain) continue;   // a bot has no client
            pushes.push({ viewer: s, bytes: pushOrThrow(table.push(op.gameId, s)) });
        }
        pushes.push({ viewer: -1, bytes: pushOrThrow(table.push(op.gameId, -1)) });
    }
    const seat = op.viewerId !== null ? table.seatOf(op.viewerId) : -1;
    const envelope = (seat >= 0 ? products.views[seat] : null) ?? products.spectator;
    return { rc, reject, detail: 0, envelope, products, seats, pushes, needsBots: products.needsBots };
}

function envelopeOrThrow(e: Uint8Array | number): Uint8Array {
    if (typeof e === 'number') throw new Error(`table envelope refused: ${tableCodeName(e, ['TABLE_E_'])}`);
    return e;
}

function pushOrThrow(e: Uint8Array | number): Uint8Array {
    if (typeof e === 'number') throw new Error(`table push refused: ${tableCodeName(e, ['TABLE_E_'])}`);
    return e;
}

// ---- commit ------------------------------------------------------------------

/**
 * commit_table with one operation's products. Returns the committed version,
 * or null when another writer committed first (the caller reloads and retries).
 */
export async function commitProducts(
    gameId: string, row: TableRow, p: TableProducts, seats: TableSeat[], dealSeed: Uint8Array | null,
): Promise<number | null> {
    const expectedVersion = row.version;
    const seedHex = p.dealtNow && dealSeed ? bareHex(dealSeed) : null;
    const stateHex = bytesToColumnHex(p.state);
    const rosterHex = p.rosterChanged ? bytesToColumnHex(p.roster) : row.rosterHex;
    const status = gameStatusLabel(p.status);
    const views = seats.flatMap((s, i) => (p.views[i]
        ? [{ player_id: s.id, view: bareHex(p.views[i]!), status }]
        : []));
    // Membership rows feed the realtime policies (gu- and chat: topics): they
    // follow the roster whenever the operation changed it.
    const membership = p.rosterChanged;
    const { data, error } = await supabaseClient.rpc('commit_table', {
        p_game_id: gameId,
        p_expected_version: expectedVersion,
        p_state: stateHex,
        p_roster: rosterHex,
        p_status: p.status,
        p_needs_bots: p.needsBots,
        p_seats: membership ? seats.filter((s) => !s.brain).map((s) => s.id) : null,
        p_bot_seats: membership ? seats.filter((s) => s.brain).map((s) => s.id) : null,
        p_logs_packed: p.logs ? bareHex(p.logs) : null,
        p_logs_reset: p.logsReset,
        p_game_seed: seedHex,
        p_views: views,
        p_spectator: bareHex(p.spectator),
        p_closed_round: p.closedRound,
    });
    if (error) {
        console.error(`[COMMIT] commit_table failed for ${gameId}:`, error);
        throw error;
    }
    const res = data as { status: 'ok' | 'conflict'; version?: number; round_epoch?: number };
    if (res.status !== 'ok' || typeof res.version !== 'number') return null;
    const version = Number(res.version);
    // Rows in play stay warm in this isolate; a lobby or a finished game is read fresh.
    noteCommittedRow(gameId, p.status === L.GAME_STATUS_PLAYING ? {
        version,
        roundEpoch: Number(res.round_epoch ?? 0),
        state: p.state,
        roster: p.roster,
        rosterHex,
        gameSeed: seedHex ?? row.gameSeed,
    } : null);
    return version;
}

// ---- broadcast ------------------------------------------------------------------

/**
 * The kernel's per-viewer pushes to each human seat's private topic and the
 * spectator topic, in one batched realtime POST.
 *
 * `b` is the kernel's push (evwire.h as3: the as2 sequence, a flags byte, the
 * new roster when the operation changed it). The envelope says `as2` until the
 * web reading as3 is deployed (Phase 5b switches `t`): a client that reads as2
 * reads the sequence prefix, which is byte for byte what it always received,
 * and drops a lobby push whose roster it does not hold by refetching.
 */
export async function broadcastPushes(
    gameId: string, version: number, seats: TableSeat[],
    pushes: { viewer: number; bytes: Uint8Array }[], reqId: string,
): Promise<void> {
    const messages: BroadcastMessage[] = pushes.map(({ viewer, bytes }) => ({
        topic: viewer >= 0 ? `gu-${gameId}-${seats[viewer].id}` : `game-${gameId}`,
        event: 'animation_events',
        payload: { t: 'as2', s: crypto.randomUUID(), v: version, b: bytesToBase64(bytes) },
    }));
    await broadcastMessages(messages, reqId);
}
