// Microbench: per-move SERVER COMPUTE on the C Table - what one human move costs
// the isolate between reading the row and the commit_table round trip.
//
//   server : one kernel section as table_io.ts runTableOp runs it - load the row,
//            set its deal seed, table_act, table_commit_products (the durable
//            state, the session-log records, every seat's envelope and the
//            spectator's), the as3 push for every viewer, and the hex the
//            commit_table call and the view rows carry.
//   floor  : load, set the deal seed and table_act alone - approximately the
//            raw C apply cost.
//
//   BENCH_MOVES=20000 BENCH_PLAYERS=4 TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/bench_packed.ts
//
// Games are dealt from pinned seeds with every seat human, and moves are picked
// at random (a pinned stream) from the kernel's legal moves of every seat still
// in; the pick, and for the floor the commit that moves the game on, are outside
// the timer. Phase 8 (docs/C_GAME_SHAPE_MIGRATION.md) moved this off the
// TypeScript Game and the retired legacy and packed pipelines it compared.
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { bytesToBareHex } from '../sdk/ts/wire/bytes.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { MemTable, residentBoard, residentMoves } from './helpers/table_mem.ts';

const MOVES = Number(process.env.BENCH_MOVES || 20000);
const PLAYERS = Number(process.env.BENCH_PLAYERS || 4);

let seed = 0xfeed;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
const ri = (n: number) => Math.floor(rnd() * n);

const table = fixtureTable();
const GID = 'bench';

type Mode = 'server' | 'floor';
function run(mode: Mode, target: number): { moves: number; ns: bigint } {
  let moves = 0; let ns = 0n; let game = 0;
  while (moves < target) {
    const dealSeed = Uint8Array.from({ length: 32 }, (_, i) => (i * 7 + game++) & 0xff);
    const t = MemTable.deal(Array.from({ length: PLAYERS }, (_, i) => ({ id: `player-${i}`, name: `P${i}` })), dealSeed, GID);
    for (let mv = 0; mv < 600 && moves < target; mv++) {
      t.load();
      if (residentBoard().status !== L.GAME_STATUS_PLAYING) break;
      const menu = Array.from({ length: PLAYERS }, (_, s) => residentMoves(s)).flat();
      if (menu.length === 0) break;
      const m = menu[ri(menu.length)];
      const actor = `player-${m.seat}`;

      const t0 = process.hrtime.bigint();
      table.load(t.state, t.roster);
      table.setDealSeed(t.seedHex);
      const rc = table.act(actor, m.wire, null, 0);
      let p = null;
      if (mode === 'server' && rc === L.TABLE_APPLIED) {
        p = table.commit(GID, t.version + 1, 1_700_000_000_000);
        if (typeof p === 'number') throw new Error(`commit products refused (${p})`);
        for (let viewer = -1; viewer < PLAYERS; viewer++) table.push(GID, viewer);
        bytesToBareHex(p.state);
        if (p.logs) bytesToBareHex(p.logs);
        for (const v of p.views) if (v) bytesToBareHex(v);
        bytesToBareHex(p.spectator);
      }
      const dt = process.hrtime.bigint() - t0;
      if (rc !== L.TABLE_APPLIED) continue; // a refused pick: skip, don't count
      ns += dt;
      moves++;
      // Move the row on, outside the timer (the table still holds the move just applied).
      t.commit();
    }
  }
  return { moves, ns };
}

// Warm up the kernel + JIT, then measure.
for (const mode of ['server', 'floor'] as Mode[]) { seed = 0xfeed; run(mode, 1500); }
console.log(`table move bench: players=${PLAYERS} moves=${MOVES} (per-move server compute, broadcast/DB excluded)`);
const out: Record<string, number> = {};
for (const mode of ['server', 'floor'] as Mode[]) {
  seed = 0xfeed;
  const { moves, ns } = run(mode, MOVES);
  const us = Number(ns) / 1000 / moves;
  out[mode] = us;
  console.log(`  ${mode.padEnd(6)} ${us.toFixed(1).padStart(7)} µs/move   ${(1e6 / us | 0).toString().padStart(8)} moves/sec`);
}
console.log(`  kernel apply share of the server's move: ${((out.floor / out.server) * 100).toFixed(0)}%`);
