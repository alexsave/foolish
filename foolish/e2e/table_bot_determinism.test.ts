/* =============================================================================
 * A bot cycle is a function of the stored row, not of the module that runs it
 * =============================================================================
 * The bot loop loads a game row into whatever bots.wasm instance the request
 * landed on: a cold isolate, a warm one that just drove other games, or another
 * edge isolate retrying a lost CAS write race. Every one of them must choose the
 * same moves, because every part of a game is a pure function of the row plus
 * its deal seed (docs/C_GAME_SHAPE_MIGRATION.md; c/src/table.c table_drive_seed).
 *
 * The Monte-Carlo brains used to break that: robusta and firecracker roll their
 * candidate moves out with policies that draw from the draw stream, which the
 * cycle seeded only as a move applied, so a decision read whatever the module's
 * earlier work left there.
 *
 * For every brain the server can seat, a game is walked cycle by cycle, and each
 * stored row is driven on two instances: a fresh one, and one that plays an
 * unrelated table's bot cycle before every row it is handed. Both must apply the
 * same actions and commit the same state, session-log records and push.
 *
 * The other half of the same policy: a decision seeded from the board ALONE
 * repeats itself on a board the game has already played, so a table whose
 * remaining players are all `random` bots could return to an exact earlier board
 * and loop there forever - about one game in 40 at 4 to 7 seats, holding
 * `needs_bots` true for good. The seed folds in the game's PROGRESS (the session
 * log's length, c/src/bot_drive.h), which only grows, so the second test here
 * plays the same all-random sweep those loops were found in and every game ends.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { createServerTable, type ServerTable } from '../sdk/ts/table/server_table.ts';
import { dealBotTable, botCycle, seedBytes, type BotTableRow } from './helpers/bot_table.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

// The seeded roster (c/src/bot_roster.c with seeded=1): what server/impls/supabase/seed.sql can seat.
const BRAINS = ['random', 'simple_heuristic', 'handwritten', 'robusta', 'firecracker', 'blackpowder', 'cordite', 'octogen'];
const CYCLES = 24;

const hex = (b: Uint8Array | null) => (b ? Buffer.from(b).toString('hex') : '-');

/** One cycle of `row` on `table`, as bot_actions.ts runCycle makes it, and every byte it produced. */
function cycleBytes(table: ServerTable, row: BotTableRow): string {
    assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
    assert.equal(table.setDealSeed(row.seedHex), L.TABLE_OK);
    assert.ok(table.setSessionLog(row.log) >= 0);
    const d = table.botDrive(null);
    assert.ok(typeof d !== 'number', `the drive runs (${d})`);
    if (d.n === 0) return 'idle';
    const moves = table.drivePrefs();
    const p = table.commit(row.gameId, row.version + 1, 1_726_600_000_000 + row.version);
    assert.ok(typeof p !== 'number', `commit products (${p})`);
    const push = table.push(row.gameId, -1);
    assert.ok(push instanceof Uint8Array, `the spectator push (${push})`);
    return `seats=${d.seats} stop=${d.stop} moves=${hex(moves)} state=${hex(p.state)} logs=${hex(p.logs)} push=${hex(push)}`;
}

test('every brain chooses the same cycle on a fresh module and on one with another history', () => {
    const fresh = createServerTable();
    const used = createServerTable();
    const busy = createServerTable();   // makes the unrelated rows; never compared
    // The unrelated work: a robusta and firecracker table, one cycle before each compared row.
    let other = dealBotTable(['robusta', 'firecracker', 'handwritten'], seedBytes(3, 99), { table: busy, gameId: 'other' });
    const unrelated = () => {
        if (other.status !== L.GAME_STATUS_PLAYING) other = dealBotTable(['robusta', 'firecracker', 'handwritten'], seedBytes(3, other.version), { table: busy, gameId: 'other' });
        const c = botCycle(other, { table: used });
        other = c.row;
    };

    const diverged: string[] = [];
    let compared = 0;
    for (const [k, brain] of BRAINS.entries()) {
        let row = dealBotTable([brain, brain, brain], seedBytes(3, 40 + k), { table: busy, gameId: `det-${brain}` });
        for (let cycle = 0; cycle < CYCLES && row.status === L.GAME_STATUS_PLAYING; cycle++) {
            const a = cycleBytes(fresh, row);
            unrelated();
            const b = cycleBytes(used, row);
            compared++;
            if (a !== b) { diverged.push(`${brain} at cycle ${cycle} (version ${row.version})`); break; }
            row = botCycle(row, { table: busy }).row;
        }
    }
    console.error(`[table_bot_determinism] ${compared} rows compared across ${BRAINS.length} brains`);
    assert.deepEqual(diverged, [], 'a stored row drives the same on every module');
    assert.ok(compared >= BRAINS.length * 10, `the games had cycles to compare (${compared})`);
});

// The sweep the loops were found in (docs/C_GAME_SHAPE_MIGRATION.md, Phase 8 final
// pass part 3): 40 seeds per seat count, every seat a `random` bot. seedBytes(4, 5)
// is the 4-seat game that looped with a 12-cycle period from cycle 105.
const SWEEP_SEATS = [4, 5, 6, 7];
const SWEEP_SEEDS = 40;
// Comfortably above the longest of these games, and far below a loop, which never
// ends at all.
//
// RE-MEASURED WHEN A GOOD BECAME A MOVE. The longest was 503 cycles when a silent
// `good` was bundled - several of them could ride one cycle and the cycle ended
// on the first VISIBLE action. classify() (c/src/bot_drive.c) prices a good as a
// move now, so a good ends its cycle like any other move and the same games take
// more, finer cycles: measured over this exact sweep, 53886 cycles carrying
// 53886 actions (one action per cycle, bundling having nothing left to bundle),
// longest 858 at rnd-7-24. Nothing about the games changed - only the unit this
// ceiling counts in - so the ceiling moves with the unit and keeps its margin.
const CYCLE_CEILING = 1400;

test('an all-random table always ends: no board can repeat its own move forever', () => {
    const table = createServerTable();
    const unfinished: string[] = [];
    let longest = 0, played = 0;
    for (const np of SWEEP_SEATS) {
        for (let s = 1; s <= SWEEP_SEEDS; s++) {
            const gameId = `rnd-${np}-${s}`;
            let row = dealBotTable(Array.from({ length: np }, () => 'random'), seedBytes(np, s), { table, gameId });
            let cycles = 0;
            for (; cycles < CYCLE_CEILING && row.status === L.GAME_STATUS_PLAYING; cycles++) {
                const c = botCycle(row, { table });
                assert.ok(c.drive.n > 0, `${gameId}: a playing table always has a bot move`);
                row = c.row;
            }
            played++;
            if (cycles > longest) longest = cycles;
            if (row.status === L.GAME_STATUS_PLAYING) unfinished.push(`${gameId} (still playing after ${cycles} cycles)`);
        }
    }
    console.error(`[table_bot_determinism] ${played} all-random games, longest ${longest} cycles`);
    assert.deepEqual(unfinished, [], `every all-random table ends within ${CYCLE_CEILING} cycles`);
    assert.ok(longest < CYCLE_CEILING, `the ceiling is a ceiling, not the answer (longest ${longest})`);
});
