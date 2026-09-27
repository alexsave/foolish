// Two runs of one game are the same run.
//
// The product's whole determinism story is that a game is a pure function of
// its deal seed: 32 crypto bytes drawn once, saved to games.game_seed, and
// everything after them derived - the deal, every draw, and every bot decision
// (the strategy streams are seeded from the board and the seed's secret base).
// That story was once quietly false in the shape nobody looks at: not the
// cards, which were always right, but the rows the game writes down, which drew
// a fresh id and a wall-clock stamp each.
//
// So this plays the SAME deal twice, from the same seed and the same commit
// clock, the way the server plays one - a C Table through the kernel's bot
// cycle (helpers/bot_table.ts) - and asserts the two runs are identical all the
// way down: the durable state blob after every cycle, the roster, the session
// log rows (games.logs_packed), and the replay code the finished game is cut
// into. The second run is on a DIFFERENT bots.wasm instance, so the game is a
// function of its seed and not of whatever the module played before it.
//
// (The TS Game's log ids, which this file also held, are gone with that Game:
// a session log record carries no id.)
//
// It needs no database. The kernel is the whole subject.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServerTable, type ServerTable } from '../sdk/ts/table/server_table.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { botCycle, dealBotTable, replayCodeOf, type BotTableRow } from './helpers/bot_table.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { suiteRng } from './helpers/rng.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const rng = suiteRng('replay_is_a_function');

// A pinned deal. Derived from the suite seed rather than typed in, so widening
// the search is `E2E_SEED_REPLAY_IS_A_FUNCTION=<n>` and not an edit.
function dealSeed(): Uint8Array {
    const r = rng.fork('deal');
    return Uint8Array.from({ length: 32 }, () => r.int(256));
}

const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');

/** Deal and play the game to its end on `table`, keeping every committed row. */
function playOnce(table: ServerTable, seed: Uint8Array): BotTableRow[] {
    let row = dealBotTable(['random', 'handwritten', 'random', 'handwritten'], seed, { table, gameId: 'rf' });
    const rows = [row];
    while (row.status === L.GAME_STATUS_PLAYING) {
        const c = botCycle(row, { table });
        assert.ok(c.drive.n > 0, `a playing game with no bot move at version ${row.version}`);
        row = c.row;
        rows.push(row);
    }
    return rows;
}

test('the same deal played twice is the same game, log rows included', () => {
    const seed = dealSeed();
    const a = playOnce(fixtureTable(), seed);
    const b = playOnce(createServerTable(), seed);
    const last = a[a.length - 1];

    assert.ok(last.log.length > 0, `the run wrote no logs, so it proves nothing (deal ${hex(seed)})`);
    assert.equal(last.status, L.GAME_STATUS_GAME_OVER, `the run did not finish (deal ${hex(seed)})`);

    // The cards first, cycle by cycle, so a failure here reads as a rules
    // problem rather than a bookkeeping one, and names the cycle it starts at.
    assert.equal(b.length, a.length, `two plays of deal ${hex(seed)} took different numbers of cycles (seed=${rng.seed}, ${rng.env})`);
    a.forEach((ra, i) => {
        assert.equal(hex(b[i].state), hex(ra.state),
            `two plays of deal ${hex(seed)} diverge at cycle ${i} (seed=${rng.seed}, ${rng.env})`);
    });
    assert.equal(hex(b[b.length - 1].roster), hex(last.roster), `roster differs (deal ${hex(seed)})`);

    // Then the rows: the session log as the table commits it, record for record.
    assert.equal(hex(b[b.length - 1].log), hex(last.log),
        `the session log ROWS differ between two plays of deal ${hex(seed)} (seed=${rng.seed}, ${rng.env})`);

    // And the whole game, as the code it is shared as.
    assert.equal(hex(replayCodeOf(b[b.length - 1], seed, { table: fixtureTable() })),
        hex(replayCodeOf(last, seed, { table: fixtureTable() })), `the replay codes differ (deal ${hex(seed)})`);
});
