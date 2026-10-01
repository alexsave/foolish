// Durable state codec: the `games.state bytea` blob is lossless.
//
// The blob is the kernel's (table.h: [TABLE_STATE_FORMAT][deterministic deck]
// [state_put], then the board's clock at v3). A table loads it (table_load: state_import, game_validate) and
// every commit writes it back (table_commit_products). Seat identity is not in
// it: that is the roster column, a separate blob.
//
// This used to play 40 seeded bots-only games (2, 3, 4 and 6 seats, ten seeds
// each) over wasm and assert two things at every committed state. The first -
// load then commit is the identity: the blob a table writes back for a board it
// only loaded is byte-identical to the blob it loaded, over thousands of states,
// every blob under 2,048 bytes - only calls C methods, so those 40 games live in
// the native C suite now: c/tests/tests.c
// test_table_state_blob_round_trips_every_reachable_state, at the same seeds and
// thresholds, for a fraction of a second instead of 71.
//
// The second cannot move: that the board read back through the GENERATED
// TypeScript accessors is the whole blob - a fixture rebuilt from those fields
// alone (table_play.ts rebuild) seals to the same bytes. That is a claim about
// sdk/ts/gen/game_layout.bots.ts reading the module's memory, which crosses the
// boundary, so it stays here over two of those games, and keeps the first
// assertion beside it so the shipped wasm is also seen to round-trip.
//
// The TS marshal this file used to hold (serializeGameState /
// deserializeGameState over a JS Game) is gone with the TS game shape.
// Pure kernel test - needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { boardBytes, fixtureTable, reasonOf } from './helpers/table_fixture.ts';
import { rebuild, residentBoard } from './helpers/table_play.ts';
import { dealBotTable, driveBotTable, seedBytes, type BotTableRow } from './helpers/bot_table.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');

test('every commit writes the durable blob at the current format', () => {
  // The switch step (c/src/view.h STATE_BLOB_FORMAT): this kernel reads v2 and
  // v3 and writes v3, the board with its clock behind it.
  const row = dealBotTable(['random', 'random', 'random', 'random'], seedBytes(4, 1));
  assert.equal(L.TABLE_STATE_FORMAT, L.TABLE_STATE_FORMAT_V3, 'the format this kernel writes');
  assert.equal(L.TABLE_STATE_CLOCK_BYTES, L.TABLE_STATE_V3_CLOCK_BYTES, 'and a written blob carries the clock');
  assert.equal(row.state[0], L.TABLE_STATE_FORMAT, 'the dealt blob leads with its format');
});

test('a blob of any other format is refused, not misread', () => {
  // v1 blobs were migrated to v2 at deploy (20260708130000_migrate_state_blobs_v2);
  // a missed one must fail loud, never load as a mis-parsed game. v2 and v3 are
  // both read - see the next case.
  const row = dealBotTable(['random', 'random', 'random', 'random'], seedBytes(4, 2));
  const table = fixtureTable();
  for (const version of [0, 1, 4, 0xff]) {
    const blob = row.state.slice();
    blob[0] = version;
    const rc = table.load(blob, row.roster);
    assert.equal(rc, L.TABLE_E_STATE_VERSION, `format ${version}: ${reasonOf(rc, ['TABLE_E_', 'GAME_INVALID_'])}`);
  }
  assert.equal(table.load(row.state, row.roster), L.TABLE_OK, 'the blob as written loads');
});

test('a v3 blob, the board with its clock behind it, loads with that clock', () => {
  // The format the switch step writes, read by this kernel now so a rollback
  // across the switch finds every row readable. The fixture is the written
  // board with the v3 format byte and a clock: shown at T, done playing at T+500.
  const T = 1_700_000_000_000;
  const row = dealBotTable(['random', 'random', 'random', 'random'], seedBytes(4, 3));
  const table = fixtureTable();
  const board = boardBytes(row.state);
  const clock = new Uint8Array(L.TABLE_STATE_V3_CLOCK_BYTES);
  for (let b = 0; b < 6; b++) {
    clock[b] = Math.floor(T / 2 ** (8 * b)) % 256;
    clock[6 + b] = Math.floor((T + 500) / 2 ** (8 * b)) % 256;
  }
  const v3 = new Uint8Array(board.length + clock.length);
  v3.set(board);
  v3.set(clock, board.length);
  v3[0] = L.TABLE_STATE_FORMAT_V3;
  assert.equal(table.load(v3, row.roster), L.TABLE_OK, 'the v3 blob loads');
  // A bots-only board waits the bots-only pace from when it was shown, so the
  // wait moves with the time gone since T: the clock was read.
  const wait = table.botWaitMs(T + 100);
  assert.ok(wait > 0, `a bot waits on the board the clock says was just shown (got ${wait})`);
  assert.equal(table.botWaitMs(T + 150), wait - 50, 'less the time gone since the clock');
  assert.equal(table.botWaitMs(T + 3_600_000), 0, 'and not at all an hour later');
  const p = table.commit(row.gameId, row.version, 0);
  assert.ok(typeof p !== 'number', 'products of the loaded v3 row');
  assert.equal(hex(p.state), hex(v3), 'written back at the format this kernel writes, as the same board and clock');
});

test('the board the generated accessors read back rebuilds the blob, at every state of two games', () => {
  const table = fixtureTable();
  let checks = 0;
  let maxBlob = 0;

  const roundTrip = (row: BotTableRow) => {
    maxBlob = Math.max(maxBlob, row.state.length);
    // 1. load then commit writes the same bytes back (the shipped module's copy of the native proof).
    const rc = table.load(row.state, row.roster);
    assert.equal(rc, L.TABLE_OK, `check ${checks}: the committed blob loads (${reasonOf(rc, ['TABLE_E_', 'GAME_INVALID_'])})`);
    const board = residentBoard(row.gameId, row.state, row.roster);
    const p = table.commit(row.gameId, row.version, 0);
    assert.ok(typeof p !== 'number', `check ${checks}: products of a loaded table`);
    assert.equal(hex(p.state), hex(row.state), `check ${checks}: load then commit is not byte-identical`);
    // 2. the fields the generated accessors read are the whole BOARD: every byte
    // of the blob but its trailing clock, which is the table's and no field of
    // the Game (a rebuilt fixture has a zero clock, never having been shown).
    const again = rebuild(board).build();
    const boardHex = (b: Uint8Array) => hex(boardBytes(b));
    assert.equal(boardHex(again.state), boardHex(row.state), `check ${checks}: the board read back does not rebuild the blob`);
    checks++;
  };

  // The first two 4-seat games of the native test's set (its seeds: seedBytes(4, 1000 + seed)).
  const np = 4;
  for (let seed = 0; seed < 2; seed++) {
    const dealt = dealBotTable(Array(np).fill('handwritten'), seedBytes(np, 1000 + seed), { gameId: `s${np}-${seed}` });
    roundTrip(dealt);
    const end = driveBotTable(dealt, { maxActions: 1, maxCycles: 20000, onCycle: (c) => roundTrip(c.row) });
    assert.equal(end.status, L.GAME_STATUS_GAME_OVER, `${np}p seed ${seed} finished`);
  }

  assert.ok(checks > 100, `expected over a hundred round-trips, ran ${checks}`);
  // The blob must stay small: it is the whole row's game.
  assert.ok(maxBlob < 2048, `durable state blob unexpectedly large: ${maxBlob} bytes`);
  console.error(`  state_codec: ${checks} round-trips, max blob ${maxBlob} bytes, v${L.TABLE_STATE_FORMAT}`);
});
