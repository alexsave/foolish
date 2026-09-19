// Regression guard for the resident game slot: a load replaces it wholesale.
//
// The bot loop runs every cycle on ONE bots.wasm instance whose resident Game is
// the table: it loads a row, drives, commits, and the next cycle (the same game
// after a CAS conflict, or another game entirely) loads over whatever the last
// one left. The TS bridge this file was written for once skipped a marshal on
// object identity and let a later decision read STALE kernel state - most
// damagingly a since-emptied deck still reading as alive, which gates off the
// exact endgame solver and throws forced wins. That bridge is gone; the property
// it broke is the kernel's now: nothing a previous table left behind (its board,
// its session log, its bots' memory, its draw streams) may reach a decision on
// the table loaded after it.
//
// Oracle: the same stored row, loaded and driven on an instance that just played
// a DIFFERENT board, must commit byte-identical products to the row loaded and
// driven on a second private instance. Any divergence is resident-slot leakage.
//
// Where the volume lives: c/tests/tests.c test_table_bot_drive_ignores_other_tables
// plays five bots-only games of 2 to 6 seats to their end in lockstep on one
// table, every load following another width's board, and holds each cycle's
// commit against the same game played alone - handwritten, simple_heuristic and
// random, over a thousand compared cycles - and test_table_bot_drive_ignores_instance_history
// does the same for every roster brain with the module's RNG streams perturbed
// between the two drives. The handwritten case here is a short run of the same
// oracle over the WASM build, because that build has its own arena, IO buffer,
// reset path and stack, and its resident game is the one the server's bot loop
// actually reuses. The cordite case keeps its full volume: what cordite leaves
// resident between tables (its sampled-world slots and its transposition table)
// is sized by defines only the wasm build sets (WORLD_LOG_CAP=40 against 0
// native, CD_TT_BITS=12 with CD_TT_2WAY and CD_TT_PACK8 against 16 plain), so a
// native run proves a differently-configured cordite and cannot stand in here.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { createServerTable, type ServerTable } from '../sdk/ts/table/server_table.ts';
import { fixtureTable, type TableFixture } from './helpers/table_fixture.ts';
import { lcg, randomPlayingBoard, type Rnd } from './helpers/kernel_board.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const hex = (b: Uint8Array | null) => (b ? Buffer.from(b).toString('hex') : '-');
const SEED_HEX = '5a'.repeat(32);

/** One bot cycle on `table` exactly as the server runs it, and what it commits. */
function drive(table: ServerTable, fx: TableFixture, gameId: string): string {
  assert.equal(table.load(fx.state, fx.roster), L.TABLE_OK, 'the row loads');
  assert.equal(table.setDealSeed(SEED_HEX), L.TABLE_OK);
  const d = table.botDrive(null, 1);
  assert.ok(typeof d !== 'number', `the drive runs (${d})`);
  if (d.n === 0) return 'no-move';
  const p = table.commit(gameId, 2, 1_700_000_000_000);
  assert.ok(typeof p !== 'number', `commit products (${p})`);
  return `seats=${d.seats.join(',')} stop=${d.stop} state=${hex(p.state)} logs=${hex(p.logs)} spectator=${hex(p.spectator)}`;
}

function board(rnd: Rnd, brain: string): TableFixture {
  for (;;) {
    const fx = randomPlayingBoard(rnd, () => brain);
    if (fx) return fx;
  }
}

function runDifferential(brain: string, N: number, seed: number) {
  const rnd = lcg(seed);
  const reused = fixtureTable();
  const fresh = createServerTable();
  let checked = 0;
  for (let i = 0; i < N; i++) {
    const target = board(rnd, brain);
    const prior = board(rnd, brain);
    const expected = drive(fresh, target, 'g');
    if (expected === 'no-move') continue;
    // The hazard: the reused instance just played another board, left its log and
    // its bots' search state resident, then loads the target row.
    drive(reused, prior, 'other');
    const actual = drive(reused, target, 'g');
    checked++;
    assert.equal(actual, expected,
      `${brain}: a table loaded after another board decided differently from a fresh instance (case ${i}, seed ${seed})`);
  }
  assert.ok(checked > N * 0.5, `${brain}: too few comparable boards (${checked}/${N})`);
}

// A short run: the native lockstep test above carries the volume for a brain
// that keeps no state of its own, and this run is for the wasm build's load path.
// Forty boards still put prior and target rows of one width and one blob length
// back to back several times, which is the shape of the bug this file exists for.
test('resident slot: a load leaves nothing of the last table to handwritten', () => {
  runDifferential('handwritten', 40, 12345);
});

test('resident slot: a load leaves nothing of the last table to cordite', () => {
  runDifferential('cordite', 250, 6789);
});
