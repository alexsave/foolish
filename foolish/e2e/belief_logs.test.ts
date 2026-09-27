// Regression guard for the belief-bot session log: the log is LOAD-BEARING.
//
// The belief/memory bots (octogen, cordite, blackpowder, ...) deduce hidden cards
// from the current session log. For a long window the server loaded state
// without it, so these bots chose blind in production and played as if they had
// no memory (see the octogen investigation). The server bot loop now hands the
// kernel the stored log (games.logs_packed) whenever the kernel says a belief bot
// is about to choose (table_bots_need_logs, then table_set_session_log).
//
// The kernel half - that from the same row, the same deal seed and the same
// position, octogen's decision with the session log differs from its decision
// with the memory taken away on a meaningful fraction of positions - lives in
// the native C suite now: c/tests/tests.c
// test_table_session_log_is_load_bearing_for_octogen, which plays the 18 games
// (2, 3 and 4 seats, 6 seeds each) this file used to play over wasm. It moved
// because it only calls C methods, and it cost 73 seconds here against 25 there.
//
// It also moved because the comparison this file made could not go red. It
// compared against an EMPTY log, and the log's record count is the progress
// term of every bot decision's seed (c/src/bot_drive.h bot_drive_seed_decision),
// so an empty log moves octogen's RNG stream and changes moves whether or not
// the records are read: with the kernel's import cut out, the count this file
// asserted to be 0 was 320 of 1,736. The native test compares against a log of
// the same record count with nothing in it, which no TypeScript can write
// without knowing the record layout, and that is not done here.
//
// What stays here is ONE of those games through the shipped bots.wasm build,
// watched by the kernel's belief probe (c/src/bot_drive.h BeliefProbe, read
// through the generated accessors): at every decision a belief bot makes, the
// board the strategy was about to read holds exactly the records the row's log
// holds, with real cards in them. The native binary and the module are built at
// different caps, so a native pass says the kernel splices the log in and reads
// it, and this says the module the server runs splices it in too. With the
// import cut out the probe sees 0 records at every decision. The wiring half -
// that the real bot loop reads and hands over the whole log - is
// e2e/belief_logs_wiring.test.ts.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { botCycle, dealBotTable, seedBytes } from './helpers/bot_table.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

test('the shipped wasm splices the session log into the board octogen reads', () => {
  const table = fixtureTable();
  let decisions = 0, blind = 0, withCards = 0;
  // The first 3-seat game of the native test's set (its seed: seedBytes(3, 0xbe11)).
  const np = 3;
  let row = dealBotTable(Array.from({ length: np }, () => 'octogen'), seedBytes(np, 0xbe11), { gameId: `belief-${np}-0` });
  for (let guard = 0; guard < 3000 && row.status === L.GAME_STATUS_PLAYING; guard++) {
    assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
    const belief = table.botsNeedLogs() && row.log.length > 0;
    // The records the row holds, as the kernel counts them.
    const records = belief ? table.setSessionLog(row.log) : 0;
    table.__beliefProbeReset();
    const cycle = botCycle(row, { maxActions: 1 });
    if (cycle.drive.n === 0) break;
    if (belief) {
      const seen = table.__beliefProbeDump();
      assert.ok(seen.length > 0, `a belief bot chose at version ${row.version} and the probe recorded no search`);
      for (const p of seen) {
        decisions++;
        if (p.nLogs !== records) blind++;
        // The session log hides drawn cards (they cross as card backs), so the
        // first search of a game sees GAME_START and draws with no card in them;
        // from the first attack on, the records carry real cards.
        if (p.cards.size > 0) withCards++;
      }
    }
    row = cycle.row;
  }

  console.error(`[belief_logs] decisions=${decisions} blind=${blind} withCards=${withCards}`);
  assert.ok(decisions > 40, `exercised ${decisions} belief decisions`);
  // If the import were ignored (the regression), every decision would be blind.
  assert.equal(blind, 0,
    `octogen searched ${blind}/${decisions} times over a board not holding the row's session log - the belief input is being ignored`);
  assert.ok(withCards > 40, `only ${withCards}/${decisions} searches saw real cards in the log`);
});
