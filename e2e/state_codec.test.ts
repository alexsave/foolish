// Durable state codec: the `games.state bytea` blob is lossless.
//
// The blob is the kernel's (table.h: [TABLE_STATE_FORMAT][deterministic deck]
// [state_put]). A table loads it (table_load: state_import, game_validate) and
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
import { fixtureTable, reasonOf } from './helpers/table_fixture.ts';
import { rebuild, residentBoard } from './helpers/table_play.ts';
import { dealBotTable, driveBotTable, seedBytes, type BotTableRow } from './helpers/bot_table.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');

test('every commit writes the durable blob at the current format', () => {
  const row = dealBotTable(['random', 'random', 'random', 'random'], seedBytes(4, 1));
  assert.equal(L.TABLE_STATE_FORMAT, 2, 'the format this kernel writes');
  assert.equal(row.state[0], L.TABLE_STATE_FORMAT, 'the dealt blob leads with its format');
});

test('a blob of any other format is refused, not misread', () => {
  // v1 blobs were migrated to v2 at deploy (20260708130000_migrate_state_blobs_v2);
  // a missed one must fail loud, never load as a mis-parsed game.
  const row = dealBotTable(['random', 'random', 'random', 'random'], seedBytes(4, 2));
  const table = fixtureTable();
  for (const version of [0, 1, 3, 0xff]) {
    const blob = row.state.slice();
    blob[0] = version;
    const rc = table.load(blob, row.roster);
    assert.equal(rc, L.TABLE_E_STATE_VERSION, `format ${version}: ${reasonOf(rc, ['TABLE_E_', 'GAME_INVALID_'])}`);
  }
  assert.equal(table.load(row.state, row.roster), L.TABLE_OK, 'the blob as written loads');
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
    // 2. the fields the generated accessors read are the whole blob.
    const again = rebuild(board).build();
    assert.equal(hex(again.state), hex(row.state), `check ${checks}: the board read back does not rebuild the blob`);
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
