// Regression guard for the belief-bot session log: the log is LOAD-BEARING.
//
// The belief/memory bots (octogen, cordite, blackpowder, ...) deduce hidden cards
// from the current session log. For a long window the server loaded state
// without it, so these bots chose blind in production and played as if they had
// no memory (see the octogen investigation). The server bot loop now hands the
// kernel the stored log (games.logs_packed) whenever the kernel says a belief bot
// is about to choose (table_bots_need_logs, then table_set_session_log).
//
// This test pins the kernel half, WITHOUT a database: on the C Table, from the
// same row, the same deal seed and the same position, octogen's decision with the
// session log differs from its decision with an empty log on a meaningful
// fraction of positions. If the imported log were being ignored again (the
// regression), that count would be 0. The wiring half - that the real bot loop
// reads and hands over the whole log, and that octogen saw it - is
// e2e/belief_logs_wiring.test.ts.
//
// Phase 8 (docs/C_GAME_SHAPE_MIGRATION.md) moved this off the TypeScript Game
// (belief_logs vs game.logs through wasmChooseMoveDirect): the log is now the
// durable session log the bot cycle imports (e2e/helpers/bot_table.ts), and a
// decision is the state blob the cycle committed.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { botCycle, dealBotTable, seedBytes } from './helpers/bot_table.ts';
import { hexOf } from './helpers/table_mem.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const NO_LOG = new Uint8Array(0);

test('the session log is load-bearing for octogen', () => {
  let compared = 0, beliefChangedMove = 0;
  for (const np of [2, 3, 4]) {
    for (let gi = 0; gi < 6; gi++) {
      let row = dealBotTable(Array.from({ length: np }, () => 'octogen'), seedBytes(np, 0xbe11 + gi), { gameId: `belief-${np}-${gi}` });
      for (let guard = 0; guard < 3000 && row.status === L.GAME_STATUS_PLAYING; guard++) {
        const table = fixtureTable();
        assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
        const withLog = botCycle(row, { maxActions: 1 });
        if (withLog.drive.n === 0) break;
        if (table.load(row.state, row.roster) === L.TABLE_OK && table.botsNeedLogs() && row.log.length > 0) {
          // The same cycle from the same row, with the memory taken away.
          const blind = botCycle({ ...row, log: NO_LOG }, { maxActions: 1 });
          if (hexOf(blind.row.state) !== hexOf(withLog.row.state)) beliefChangedMove++;
          compared++;
        }
        row = withLog.row;
      }
    }
  }

  console.error(`[belief_logs] compared=${compared} beliefChangedMove=${beliefChangedMove}`);
  assert.ok(compared > 500, `exercised ${compared} decisions`);
  // If the log were ignored (the regression), this would be exactly 0.
  assert.ok(beliefChangedMove > 0,
    `session log never changed octogen's move (${beliefChangedMove}/${compared}) - belief input is being ignored`);
});
