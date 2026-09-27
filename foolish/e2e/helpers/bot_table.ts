// bot_table.ts - a table of bots, dealt from a seed and played to its end by
// the kernel, the way the server plays one (docs/C_GAME_SHAPE_MIGRATION.md Phase 8).
//
// The tests and harnesses that used to play a TypeScript Game through
// start_game / processBotAction / game_done play a table instead:
//
//   const g = playBotTable(['cordite', 'octogen'], seedBytes(2, 7));
//   g.code    // the verified v6 replay code of the finished game
//   g.state   // the final durable state blob, g.roster its roster, g.log its session log
//
// Every step is a C Table call over bots.wasm (sdk/ts/table/server_table.ts):
// the lobby is a kernel-sealed fixture, the deal is table_ready with the seed,
// each cycle is exactly the server's bot cycle (bot_actions.ts runCycle: load,
// set the deal seed, import the session log when a bot reads it, table_bot_drive,
// commit the products), and the code is table_replay_code, the finalize path's
// own round-tripped encoder. Nothing here knows a byte layout or a game rule:
// whose turn it is, what a bot plays and when the game is over are the kernel's.
//
// Pure kernel: no Postgres, so the memory, stack and performance harnesses can
// import it too. Tests and harnesses only (it holds the unmasked state blob).

import * as L from '../../sdk/ts/gen/game_layout.bots.ts';
import { tableCodeName, type ServerTable, type TableDrive } from '../../sdk/ts/table/server_table.ts';
import { fixture, fixtureExports, fixtureTable } from './table_fixture.ts';

/** The 32-byte deal seed of test game `s` at `np` seats (the seeds the frozen games in seeded_codes.ts were dealt on). */
export const seedBytes = (np: number, s: number): Uint8Array =>
    new Uint8Array(Array.from({ length: 32 }, (_, i) => (i * 31 + s * 13 + np) & 0xff));

const bareHex = (b: Uint8Array): string => Buffer.from(b).toString('hex');

/** A stored table as the bot loop holds it between cycles. */
export interface BotTableRow {
    gameId: string;
    version: number;
    state: Uint8Array;
    roster: Uint8Array;
    /** games.game_seed: the deal seed as hex. */
    seedHex: string;
    /** games.logs_packed: the session log. */
    log: Uint8Array;
    /** GAME_STATUS_* of `state`. */
    status: number;
    /** The seat that lost, once the game is over; -1 before. */
    fool: number;
}

export interface BotTableOptions {
    /** The table to play on (default: the fixtures' own, fixtureTable()). */
    table?: ServerTable;
    gameId?: string;
    /** Seat names (default P1, P2, ...). */
    names?: string[];
    /** The commit clock: the first cycle's time; each later commit is one second on (default 1,700,000,000,000 ms). */
    startMs?: number;
}

function refused(what: string, rc: number): Error {
    return new Error(`bot_table: ${what} refused: ${tableCodeName(rc, ['TABLE_E_', 'GAME_INVALID_', 'ROSTER_E_', 'REPLAY_E'])} (${rc})`);
}

function commitRow(table: ServerTable, row: BotTableRow, nowMs: number): BotTableRow {
    const p = table.commit(row.gameId, row.version + 1, nowMs);
    if (typeof p === 'number') throw refused('commit products', p);
    const log = p.logs === null ? row.log : p.logsReset ? p.logs : concat(row.log, p.logs);
    return { ...row, version: row.version + 1, state: p.state, roster: p.roster, log, status: p.status, fool: p.fool };
}

function concat(a: Uint8Array, b: Uint8Array): Uint8Array {
    const out = new Uint8Array(a.length + b.length);
    out.set(a, 0);
    out.set(b, a.length);
    return out;
}

/** A lobby of `brains` (bot_roster keys), every seat ready, dealt from `seed` by table_ready. */
export function dealBotTable(brains: string[], seed: Uint8Array, opts: BotTableOptions = {}): BotTableRow {
    const table = opts.table ?? fixtureTable();
    const gameId = opts.gameId ?? 'g';
    const seats = brains.map((brain, i) => ({ id: `seat-${i}`, name: opts.names?.[i] ?? `P${i + 1}`, brain }));
    const lobby = fixture().title(gameId).seats(seats).build();
    let rc = table.load(lobby.state, lobby.roster);
    if (rc < 0) throw refused('lobby load', rc);
    rc = table.ready(seats[0].id, seed);
    if (rc !== L.TABLE_OK) throw refused('ready', rc);
    const row: BotTableRow = {
        gameId, version: 0, state: lobby.state, roster: lobby.roster, seedHex: bareHex(seed),
        log: new Uint8Array(0), status: L.GAME_STATUS_WAITING, fool: -1,
    };
    return commitRow(table, row, opts.startMs ?? 1_700_000_000_000);
}

/** One committed bot cycle, as the server's runCycle makes it. `drive.n` is 0 when no bot had work (nothing committed). */
export function botCycle(row: BotTableRow, opts: BotTableOptions & { maxActions?: number; nowMs?: number } = {}): { row: BotTableRow; drive: TableDrive; ms: number } {
    const table = opts.table ?? fixtureTable();
    let rc = table.load(row.state, row.roster);
    if (rc < 0) throw refused('load', rc);
    rc = table.setDealSeed(row.seedHex);
    if (rc < 0) throw refused('deal seed', rc);
    // Always, as the server's cycle does: the log's length is the progress term
    // of every bot decision's seed (c/src/bot_drive.h).
    rc = table.setSessionLog(row.log);
    if (rc < 0) throw refused('session log', rc);
    const t0 = performance.now();
    const drive = table.botDrive(null, opts.maxActions ?? 0);
    const ms = performance.now() - t0;
    if (typeof drive === 'number') throw refused('bot drive', drive);
    if (drive.n === 0) return { row, drive, ms };
    return { row: commitRow(table, row, opts.nowMs ?? (1_700_000_000_000 + row.version * 1000)), drive, ms };
}

/** Drives a dealt row to its end, cycle by cycle. `onCycle` sees every cycle that applied an action. */
export function driveBotTable(
    row: BotTableRow,
    opts: BotTableOptions & { maxActions?: number; maxCycles?: number; onCycle?: (c: { drive: TableDrive; ms: number; row: BotTableRow }) => void } = {},
): BotTableRow {
    const start = opts.startMs ?? 1_700_000_000_000;
    for (let cycle = 0; cycle < (opts.maxCycles ?? 5000); cycle++) {
        if (row.status !== L.GAME_STATUS_PLAYING) return row;
        const c = botCycle(row, { ...opts, nowMs: start + row.version * 1000 });
        if (c.drive.n === 0) throw new Error(`bot_table: ${row.gameId} is playing at version ${row.version} and no bot has a move`);
        row = c.row;
        opts.onCycle?.(c);
    }
    throw new Error(`bot_table: ${row.gameId} did not end in ${opts.maxCycles ?? 5000} cycles`);
}

/** The verified v6 replay code of a finished row (table_replay_code, the finalize path's encoder). */
export function replayCodeOf(row: BotTableRow, seed: Uint8Array, opts: BotTableOptions = {}): Uint8Array {
    const table = opts.table ?? fixtureTable();
    const rc = table.load(row.state, row.roster);
    if (rc < 0) throw refused('load', rc);
    const code = table.replayCode(seed, row.log);
    if (typeof code === 'number') throw refused('replay code', code);
    return code;
}

/** One action of a bot cycle: the seat, MOVE_*, and its cards as { suit, value } (covers name their attacks). */
export interface DrivenMove {
    seat: number;
    type: number;
    cards: { suit: number; value: number }[];
    attacks: { suit: number; value: number }[];
}

/**
 * The actions the last cycle on the FIXTURES' table applied (table_bot_drive's
 * BotDriveOut), read through the generated accessors. Only for the default
 * table: another instance's drive lives in its own memory.
 */
export function lastDriveMoves(): DrivenMove[] {
    const ex = fixtureExports() as unknown as { memory: WebAssembly.Memory; wasm_table_drive_ptr(): number };
    const m = L.memOf(ex.memory.buffer);
    const d = ex.wasm_table_drive_ptr();
    const card = (p: number) => ({ suit: L.Card_get_suit(m, p), value: L.Card_get_value(m, p) });
    return Array.from({ length: L.BotDriveOut_get_n(m, d) }, (_, i) => {
        const a = L.BotDriveOut_actions_at(d, i);
        const mv = L.BotDriveAction_move_at(a);
        const n = L.LegalMove_get_n_cards(m, mv);
        const type = L.LegalMove_get_type(m, mv);
        return {
            seat: L.BotDriveAction_get_seat(m, a), type,
            cards: Array.from({ length: n }, (_, j) => card(L.LegalMove_cards_at(mv, j))),
            attacks: type === L.MOVE_COVER ? Array.from({ length: n }, (_, j) => card(L.LegalMove_attack_cards_at(mv, j))) : [],
        };
    });
}

export interface PlayedBotTable extends BotTableRow {
    seed: Uint8Array;
    /** The verified v6 replay code of the finished game. */
    code: Uint8Array;
    /** Actions the bots applied. */
    actions: number;
}

/** A seeded bots-only game, dealt, played to its end and encoded, all by the kernel. */
export function playBotTable(brains: string[], seed: Uint8Array, opts: BotTableOptions & { maxActions?: number } = {}): PlayedBotTable {
    let actions = 0;
    const end = driveBotTable(dealBotTable(brains, seed, opts), { ...opts, onCycle: (c) => { actions += c.drive.n; } });
    return { ...end, seed, code: replayCodeOf(end, seed, opts), actions };
}
