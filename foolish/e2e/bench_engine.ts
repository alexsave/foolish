// Microbench: raw rules-engine throughput - full bot games simulated in-process
// on the C Table the server plays (sdk/ts/table/server_table.ts over bots.wasm),
// no DB, no broadcast:
//
//   BENCH_GAMES=400 BENCH_PLAYERS=4 BENCH_STRATEGY=random \
//     TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/bench_engine.ts
//
// Each game is a lobby of bots dealt from its own seed (table_ready), then
// driven one action per table_bot_drive on the loaded table until the kernel
// says the game ended - the bot loop's work without the commit products. Reports
// games/sec, actions/sec (an "action" = one applied bot move), and legal-move
// evaluations/sec (the kernel's enumerator, legal.c, asked for every seat still
// in before each action) - the costs every server move and every bot turn pays.
// scripts/collect_metrics.mjs reads the last three lines.
//
// Phase 8 (docs/C_GAME_SHAPE_MIGRATION.md) moved it off the TypeScript Game
// (start_game, calculateLegalMoves, processBotAction, game_done); numbers before
// and after that commit measure different paths.
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixtureExports, fixtureTable } from './helpers/table_fixture.ts';
import { dealBotTable, seedBytes } from './helpers/bot_table.ts';

if (!process.env.E2E_VERBOSE) {
  console.log = () => {};
  console.warn = () => {};
  console.info = () => {};
}
const out = (s: string) => process.stdout.write(s + '\n');

const GAMES = Number(process.env.BENCH_GAMES || 400);
const PLAYERS = Number(process.env.BENCH_PLAYERS || 4);
const STRATEGY = process.env.BENCH_STRATEGY || 'random';
// Random play can circle forever (attack, take, attack, take); such a game stops here unfinished.
const MAX_ACTIONS = 5000;

const table = fixtureTable();
const ex = fixtureExports();

/** One game: its actions and legal-move evaluations, and whether it ended. */
function play(n: number, counts: { actions: number; legalEvals: number }): boolean {
  const row = dealBotTable(Array.from({ length: PLAYERS }, () => STRATEGY), seedBytes(PLAYERS, n), { gameId: 'bench' });
  table.load(row.state, row.roster);
  table.setDealSeed(row.seedHex);
  const m = L.memOf(ex.memory.buffer);
  const g = ex.wasm_game_ptr_internal();
  for (let a = 0; a < MAX_ACTIONS; a++) {
    for (let s = 0; s < PLAYERS; s++) {
      if (L.Player_get_status(m, L.Game_players_at(g, s)) !== L.PLAYER_STATUS_IN) continue;
      ex.wasm_legal_moves(s);
      counts.legalEvals++;
    }
    const d = table.botDrive(null, 1);
    if (typeof d === 'number' || d.n === 0) return false;
    counts.actions += d.n;
    if (d.stop === L.BOT_STOP_ENDED) return true;
  }
  return false;
}

function main() {
  // Warmup (JIT / wasm instantiation) outside the timed window.
  for (let w = 0; w < 5; w++) play(1_000_000 + w, { actions: 0, legalEvals: 0 });

  const counts = { actions: 0, legalEvals: 0 };
  let finished = 0;
  const t0 = performance.now();
  for (let n = 0; n < GAMES; n++) if (play(n, counts)) finished++;
  const dt = (performance.now() - t0) / 1000;

  out(`engine bench: strategy=${STRATEGY} players=${PLAYERS} games=${GAMES}`);
  out(`  finished:    ${finished}/${GAMES}`);
  out(`  wall:        ${dt.toFixed(2)}s`);
  out(`  games/sec:   ${(GAMES / dt).toFixed(1)}`);
  out(`  actions/sec: ${(counts.actions / dt).toFixed(0)}  (${counts.actions} total)`);
  out(`  legal-evals/sec: ${(counts.legalEvals / dt).toFixed(0)}  (${counts.legalEvals} total)`);
}

main();
