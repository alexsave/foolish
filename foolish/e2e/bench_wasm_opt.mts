// Perf/mem harness for the wasm bot module: plays deterministic cordite-vs-
// cordite games through the bot cycle the server runs (table_bot_drive on the C
// Table, e2e/helpers/bot_table.ts, one decision per cycle) and reports
// ns/decision + peak linear-memory MB. Deal seeds are pinned and every decision
// is seeded from its game's deal seed, so every build variant runs the
// byte-identical workload (same deals, same MC rollouts) - the only thing that
// changes between runs is the swapped-in bots.wasm.
//
// Phase 8 (docs/C_GAME_SHAPE_MIGRATION.md) moved it off the TypeScript Game and
// wasmChooseMoveDirect: a decision's time is now the drive alone (choose and
// apply, in C), with no JS marshal around it.
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { botCycle, dealBotTable, seedBytes } from './helpers/bot_table.ts';

const __log = console.log.bind(console);
console.log = () => {}; console.warn = () => {}; console.info = () => {};

const PCS = [2, 4, 6, 8];
const GAMES = 2;
const table = fixtureTable();

let decisions = 0, totalMs = 0, peakMB = 0;
const wall0 = process.hrtime.bigint();
for (const np of PCS) {
  for (let gi = 0; gi < GAMES; gi++) {
    let row = dealBotTable(Array.from({ length: np }, () => 'cordite'), seedBytes(np, 0xdea1 + gi), { gameId: 'bench' });
    for (let guard = 0; guard < 2000 && row.status === L.GAME_STATUS_PLAYING; guard++) {
      const c = botCycle(row, { maxActions: 1 });
      if (c.drive.n === 0) break;
      totalMs += c.ms;
      decisions += c.drive.n;
      row = c.row;
    }
    const mb = Math.round(table.memoryBytes() / 1048576); if (mb > peakMB) peakMB = mb;
  }
}
const wallMs = Math.round(Number(process.hrtime.bigint() - wall0) / 1e6);
__log('BENCH ' + JSON.stringify({ decisions, ns_per_decision: Math.round((totalMs * 1e6) / decisions), peakMB, wallMs }));
