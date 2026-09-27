// M8 overlay interleave gate (docs/BOTS_WASM_MEMORY_PLAN.md).
//
// bots.wasm aliases its replay-call scratch (g_rec / g_bn / g_replay_io,
// 90.5 KiB) INTO the solver arena solve_ws - the two are never live at once
// (a bot decision vs the replay encode/decode exports are non-nesting
// top-level exports). This test is the one check that aliasing uniquely needs:
// it drives BOTH families on the SAME bots.wasm instance, interleaved, and
// proves neither corrupts the other. If the overlay offsets ever collide with
// live solver state - or a future edit makes replay read before it writes -
// the encode after a solver burst diverges and this fails.
//
// Both families run on the one instance the web's replay reads use (bots.ts,
// the module behind sdk/ts/table/client_table.ts): the solver through a C Table
// over that instance's exports (the server's bot cycle), the encode through the
// finalize path's table_replay_code, and the decode through the replay step
// index and summary (replay_steps.c), which is what a replay screen decodes.
//
// (The straight wire-format correctness of the overlaid codec is covered by
// replay_codec.test.ts; this file adds only the interleave dimension.)
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { __clientKernelExports, replayStepIndex, replaySummary } from '../sdk/ts/wasm/bots.ts';
import { ServerTable, type TableExports } from '../sdk/ts/table/server_table.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { botCycle, dealBotTable, playBotTable, replayCodeOf, seedBytes, type BotTableRow } from './helpers/bot_table.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

// The shared bots.wasm instance, as a C Table: every call below lands on it.
const table = new ServerTable(__clientKernelExports() as unknown as TableExports);

/** Up to `cycles` bot cycles of a dealt row on the shared instance. */
function drive(row: BotTableRow, cycles: number): BotTableRow {
  for (let i = 0; i < cycles && row.status === L.GAME_STATUS_PLAYING; i++) {
    const c = botCycle(row, { table });
    if (c.drive.n === 0) break;
    row = c.row;
  }
  return row;
}

// A burst of solver decisions on fresh games - this is what writes all over
// solve_ws (and therefore all over the aliased replay scratch) between the two
// encodes below. Use the heaviest MC families so the endgame solver actually
// runs and fills the arena - every one of these must be a bot bots.wasm really
// links, or the table refuses it (TABLE_E_UNKNOWN_BRAIN) rather than hammering
// nothing.
let burst = 0;
function hammerSolver(): void {
  for (const brain of ['octogen', 'blackpowder', 'cordite']) {
    drive(dealBotTable([brain, brain], seedBytes(2, 7000 + burst++), { table }), 400);
  }
}

test('M8: replay encode is byte-identical before and after a solver burst on the same instance', () => {
  // A finished game gives a non-trivial replay stream to encode.
  const seed = seedBytes(2, 81);
  const game = playBotTable(['octogen', 'octogen'], seed, { table });
  assert.equal(game.status, L.GAME_STATUS_GAME_OVER, 'could not produce a finished game to encode');

  // The production producer: the kernel re-derives the deal from the game's
  // seed and reads the actions out of its session log, so one call writes the
  // whole choice log (g_rec) and bignum (g_bn) - which is the aliased memory
  // this test exists to watch. The input is the same bytes every time, so the
  // ONLY way two encodes of it differ is corruption.
  const encode = () => Buffer.from(replayCodeOf(game, seed, { table })).toString('hex');

  const e1 = encode();                            // encode #1
  assert.equal(e1, Buffer.from(game.code).toString('hex'), 'a re-encode reproduces the code the game was cut with');
  hammerSolver();                                 // scribble all over solve_ws == the replay scratch
  const e2 = encode();                            // encode #2 - same input, post-burst

  assert.equal(e2, e1, 'encode diverged after a solver burst - overlay corruption');

  // And the reverse direction: a decode sandwiched by solver work must be stable
  // (proves decode re-inits its scratch each call too).
  const decode = () => ({ steps: replayStepIndex(game.code), summary: replaySummary(game.code) });
  const d1 = decode();
  assert.ok(d1.steps.length > 1 && d1.summary, 'the code decodes to steps');
  hammerSolver();
  const d2 = decode();
  assert.equal(d2.steps.length, d1.steps.length, 'decode length diverged after a solver burst');
  assert.deepEqual(d2.steps, d1.steps, 'decode stream diverged after a solver burst - overlay corruption');
  assert.deepEqual(d2.summary, d1.summary, 'decoded summary diverged after a solver burst');

  // Interleave the two families move-by-move: encode, choose, encode, choose ...
  // and assert the encode never budges. This is the tightest form of the check.
  for (let k = 0; k < 3; k++) {
    let row = dealBotTable(['blackpowder', 'blackpowder'], seedBytes(2, 9000 + k), { table });
    for (let step = 0; step < 12 && row.status === L.GAME_STATUS_PLAYING; step++) {
      row = drive(row, 1);
      assert.equal(encode(), e1, `encode diverged mid-game at burst ${k} step ${step}`);
    }
  }
});
