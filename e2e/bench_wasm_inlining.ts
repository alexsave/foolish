// HELPER, not a test (no `.test.ts` suffix => the e2e glob skips it). Supports
// docs/WASM_OPT_INLINING_PROTOTYPE.md.
//
// Micro-benchmark: total wall time spent deciding for a heavy MC bot (octogen),
// driving full games through the bot cycle the server runs (table_bot_drive on
// the C Table, e2e/helpers/bot_table.ts, one decision per cycle). Deterministic
// (pinned deal seeds, every decision seeded from its game's), so baseline vs
// wasm-opt'd modules do the SAME work - only codegen differs. Run in separate
// processes, swapping bots.wasm.gz between runs.
//
//   GAMES=3 TSX_TSCONFIG_PATH=e2e/tsconfig.json \
//     node --import tsx e2e/bench_wasm_inlining.ts 2>/dev/null | grep '^{'
//
// Phase 8 (docs/C_GAME_SHAPE_MIGRATION.md) moved it off the TypeScript Game and
// wasmChooseMove: choose_ms is now the drive alone (choose and apply, in C).
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { botCycle, dealBotTable, seedBytes } from './helpers/bot_table.ts';

const PLAYER_COUNTS = [2, 3, 4];
const GAMES_PER_COUNT = Number(process.env.GAMES ?? 8);

let chooseMs = 0;
let decisions = 0;

const t0 = process.hrtime.bigint();
for (const np of PLAYER_COUNTS) {
  for (let gi = 0; gi < GAMES_PER_COUNT; gi++) {
    let row = dealBotTable(Array.from({ length: np }, () => 'octogen'), seedBytes(np, 0xb07 + gi), { gameId: 'bench' });
    for (let guard = 0; guard < 3000 && row.status === L.GAME_STATUS_PLAYING; guard++) {
      const c = botCycle(row, { maxActions: 1 });
      if (c.drive.n === 0) break;
      chooseMs += c.ms;
      decisions += c.drive.n;
      row = c.row;
    }
  }
}
const wall_ms = Number(process.hrtime.bigint() - t0) / 1e6;
console.log(JSON.stringify({ decisions, choose_ms: +chooseMs.toFixed(1), wall_ms: +wall_ms.toFixed(1), ns_per_decision: Math.round((chooseMs * 1e6) / decisions) }));
