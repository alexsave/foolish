/* =============================================================================
 * The C Table's bot cycle: deterministic, retryable, and it ends in a true code
 * =============================================================================
 * docs/C_GAME_SHAPE_MIGRATION.md 2.7 and 2.8. This file held the C Table's bot
 * cycle and game end byte for byte against the TS path they replaced
 * (wasmBotDrive over a JS Game, verifyRoundTripV6FromGame, encodeExtrasBytes).
 * That TS path is deleted with the TS game shape; its last byte-for-byte record is
 * table_parity's goldens (e2e/fixtures/table_parity/golden.json, recorded from
 * the TS pipeline). What stays here is what the cycle must be on its own terms,
 * on seeded deals of 2 to 5 seats with belief and non-belief brains, one human
 * seat whose moves go through table_act, and a bots-only table:
 *
 *   - a cycle is deterministic: a second private instance running the same
 *     operations commits the same bytes (state, session-log records, pushes);
 *   - a CAS retry offered the lost attempt's moves (table_drive_prefs) replays
 *     them exactly, even under a different deal seed where a re-search would differ;
 *   - the finished game's session log encodes a verified replay code whose summary
 *     is the game the table played, and extras naming the roster's seats; a log
 *     that is not the game's is refused.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { createServerTable, type ServerTable } from '../sdk/ts/table/server_table.ts';
import { kernelReplayExtrasDecode, replaySummary } from '../sdk/ts/wasm/bots.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { legalMoves, residentBoard } from './helpers/table_play.ts';
import { type BotTableRow } from './helpers/bot_table.ts';
import { dealTable, tableMove } from './helpers/kernel_board.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const hex = (b: Uint8Array | null) => (b ? Buffer.from(b).toString('hex') : '-');
const NOW0 = 1_726_600_000_000;

interface Cycle { n: number; seats: number[]; stop: number; delay: number; bytes: string; prefs: Uint8Array; row: BotTableRow }

/** One bot cycle on `table`, exactly as bot_actions.ts runCycle makes it: null when no bot had work. */
function cycleOn(table: ServerTable, row: BotTableRow, seedHex: string, prefs: Uint8Array | null): Cycle | null {
    assert.equal(table.load(row.state, row.roster), L.TABLE_OK, `${row.gameId}: the row loads`);
    assert.equal(table.setDealSeed(seedHex), L.TABLE_OK);
    if (table.botsNeedLogs()) assert.ok(table.importSessionLog(row.log) >= 0, `${row.gameId}: the session log imports`);
    const d = table.botDrive(prefs);
    assert.ok(typeof d !== 'number', `${row.gameId}: the drive runs (${d})`);
    if (d.n === 0) return null;
    const delay = table.cycleDelayMs();
    const nextPrefs = table.drivePrefs();
    const p = table.commit(row.gameId, row.version + 1, NOW0 + row.version);
    assert.ok(typeof p !== 'number', `${row.gameId}: commit products (${p})`);
    const seats = table.seats();
    const pushes = [...seats.flatMap((s, i) => (s.brain ? [] : [i])), -1].map((v) => {
        const b = table.push(row.gameId, v);
        assert.ok(b instanceof Uint8Array, `${row.gameId}: push ${v} (${b})`);
        return hex(b);
    });
    const log = p.logs === null ? row.log : p.logsReset ? p.logs : new Uint8Array(Buffer.concat([row.log, p.logs]));
    return {
        n: d.n, seats: d.seats, stop: d.stop, delay, prefs: nextPrefs,
        bytes: `state=${hex(p.state)} logs=${hex(p.logs)} ended=${p.ended} events=${p.nEvents} pushes=${pushes.join('|')}`,
        row: { ...row, version: row.version + 1, state: p.state, roster: p.roster, log, status: p.status, fool: p.fool },
    };
}

const counts = { cycles: 0, logged: 0, actions: 0, humanMoves: 0, retries: 0, games: 0, codes: 0 };

const GAMES: (string | null)[][] = [
    [null, 'blackpowder'],
    [null, 'cordite', 'random'],
    [null, 'robusta', 'handwritten', 'firecracker'],
    [null, 'random', 'firecracker', 'blackpowder', 'simple_heuristic'],
    ['random', 'robusta', 'handwritten'],
];

/**
 * Every game played to its end and encoded on `table`, and what each step
 * committed. robusta and firecracker sample the draw stream the module's last
 * apply left, so a cycle is a function of the row AND the module's history: the
 * two instances compared below run exactly the same operations in the same order.
 */
function playAll(table: ServerTable, tally: boolean): string[] {
    const transcript: string[] = [];
    for (let k = 0; k < GAMES.length; k++) {
        const label = `game ${k} (${GAMES[k].map((b) => b ?? 'human').join(', ')})`;
        const seats = GAMES[k].map((b, i) => ({
            id: `00000000-0000-4000-8000-${String(k * 16 + i).padStart(12, '0')}`,
            name: ['Дмитрий', 'Zoë 🃏', 'B2', '田中花子', 'B4'][i], brain: b ?? '',
        }));
        const seed = Uint8Array.from({ length: 32 }, (_, i) => (i * 37 + k * 11 + 5) & 0xff);
        let row = dealTable(seats, seed, { gameId: `tbp${k}`, title: `Bot parity ${k}`, nowMs: NOW0, table });
        transcript.push(`${label} deal ${hex(row.state)} ${hex(row.log)}`);
        const human = seats.findIndex((s) => !s.brain);

        for (let step = 0; step < 3000 && row.status === L.GAME_STATUS_PLAYING; step++) {
            assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
            const needsBots = table.needsBots();
            if (tally && table.botsNeedLogs()) counts.logged++;
            if (needsBots) {
                const c = cycleOn(table, row, row.seedHex, null);
                if (c) {
                    transcript.push(`${label} step ${step} bots ${c.seats} stop=${c.stop} delay=${c.delay} ${c.bytes}`);
                    if (step % 7 === 3) {
                        // This attempt lost its CAS: the retry is offered what it chose, under another seed.
                        const r = cycleOn(table, row, 'ff'.repeat(32), c.prefs);
                        assert.ok(r, `${label} step ${step}: the retry drives`);
                        assert.deepEqual([r.seats, r.bytes], [c.seats, c.bytes], `${label} step ${step}: the retry replays the lost attempt's moves`);
                        if (tally) counts.retries++;
                    }
                    if (tally) { counts.cycles++; counts.actions += c.n; }
                    row = c.row;
                    continue;
                }
            }
            // No bot had work: the human seat moves (its first legal move, not a pickup when it has another).
            assert.ok(human >= 0, `${label} step ${step}: someone can move`);
            assert.equal(fixtureTable().load(row.state, row.roster), L.TABLE_OK);
            const moves = legalMoves(residentBoard(row.gameId, row.state, row.roster), (_, i) => i === human);
            assert.ok(moves.length > 0, `${label} step ${step}: the human has a move`);
            const m = moves[moves.length > 1 && moves[0].kind === 'pickup' ? 1 : 0];
            const next = tableMove(row, m.playerId, m.wire, { nowMs: NOW0 + row.version, table });
            assert.ok(typeof next !== 'number', `${label} step ${step}: the human move applies (${next})`);
            row = next;
            transcript.push(`${label} step ${step} human ${hex(row.state)} ${hex(row.log)}`);
            if (tally) counts.humanMoves++;
        }
        assert.equal(row.status, L.GAME_STATUS_GAME_OVER, `${label}: played to its end`);

        // The game end: the verified v6 code and the extras blob.
        assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
        const code = table.replayCode(seed, row.log);
        assert.ok(code instanceof Uint8Array, `${label}: the replay code verifies (${code})`);
        const sum = replaySummary(code);
        assert.ok(sum, `${label}: the code decodes`);
        assert.equal(sum.numPlayers, seats.length, `${label}: the code's seats`);
        assert.equal(sum.fool, row.fool, `${label}: the code's fool is the table's`);
        assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
        const extras = table.replayExtras(row.log);
        assert.ok(extras instanceof Uint8Array, `${label}: the extras (${extras})`);
        const decoded = kernelReplayExtrasDecode(extras, seats.length, sum.moves);
        assert.deepEqual(decoded.names, seats.map((s) => s.name), `${label}: the extras name the roster's seats`);
        assert.equal(decoded.moveGaps?.length, sum.moves, `${label}: the extras time every move`);
        // A session log or a seed that is not this game's does not encode it.
        assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
        assert.equal(typeof table.replayCode(seed, row.log.slice(0, row.log.length >> 1)), 'number', `${label}: half the log is refused`);
        assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
        assert.equal(typeof table.replayCode(Uint8Array.from(seed, (b) => b ^ 1), row.log), 'number', `${label}: another deal seed is refused`);
        transcript.push(`${label} end ${hex(code)} ${hex(extras)}`);
        if (tally) { counts.games++; counts.codes++; }
    }
    return transcript;
}

test('bot cycles, CAS retries and the game end on the C Table', () => {
    const played = playAll(fixtureTable(), true);
    // The same operations on a second private instance commit the same bytes, step by step.
    const twin = playAll(createServerTable(), false);
    assert.equal(twin.length, played.length, 'the twin took as many steps');
    for (let i = 0; i < played.length; i++) assert.equal(twin[i], played[i], `step ${i}: a cycle is a function of the row and the module's history`);
    console.error(`[table_bot_parity] ${JSON.stringify(counts)}`);
    assert.ok(counts.logged > 0 && counts.retries > 0 && counts.humanMoves > 0 && counts.codes === GAMES.length,
        `every path was exercised (${JSON.stringify(counts)})`);
});

test('seat_of: the loaded roster\'s seat for an id, exactly', () => {
    const seats = ['00000000-0000-4000-8000-000000000090', 'b1', 'b2'].map((id, i) => ({ id, name: `S${i}`, brain: i ? 'random' : '' }));
    const row = dealTable(seats, new Uint8Array(32).fill(9));
    const table = fixtureTable();
    assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
    assert.equal(table.seatOf(seats[0].id), 0);
    assert.equal(table.seatOf(seats[2].id), 2);
    assert.equal(table.seatOf(seats[0].id.slice(0, 20)), -1, 'a prefix is no seat');
    assert.equal(table.seatOf(''), -1, 'the empty id is no seat');
    assert.equal(table.seatOf('stranger'), -1);
});
