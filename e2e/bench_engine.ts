// Microbench: raw rules-engine throughput — full random games simulated
// in-process through the REAL shipped modules (start_game, calculateLegalMoves,
// processBotAction, game_done), no DB, no broadcast. Run on `main` (pure-TS
// rules) and on the WASM branch (C kernel behind the same API) with the same
// env to compare:
//
//   BENCH_GAMES=400 BENCH_PLAYERS=4 node --import tsx e2e/bench_engine.ts
//
// Reports games/sec, actions/sec (an "action" = one accepted bot move), and
// legal-move evaluations/sec — the three costs every server move and every
// bot turn pays.
import { game_done } from '../server/api/common/common_utils.ts';
import { start_game } from '../server/api/common/game_lifecycle.ts';
import {
  Game,
  PrivatePlayer,
  PLAYER_STATUS,
  GAME_STATUS,
  STRATEGY_KEY,
  StrategyKey,
} from '../server/api/core/types.ts';
import {
  shouldBotActCore,
  processBotAction,
} from '../server/api/common/pure_bot_actions.ts';
import { calculateLegalMoves } from '../server/api/common/bot_strategy.ts';

if (!process.env.E2E_VERBOSE) {
  console.log = () => {};
  console.warn = () => {};
  console.info = () => {};
}
const out = (s: string) => process.stdout.write(s + '\n');

const GAMES = Number(process.env.BENCH_GAMES || 400);
const PLAYERS = Number(process.env.BENCH_PLAYERS || 4);
const STRATEGY = (process.env.BENCH_STRATEGY || 'random') as StrategyKey;

const mkPlayer = (i: number): PrivatePlayer => ({
  player_id: `bot_${i}`,
  name: `Bot ${i}`,
  status: PLAYER_STATUS.READY,
  is_ai: true,
  hand: [],
  awaiting_attack: false,
  hand_length: 0,
  strategy_key: STRATEGY,
});

const mkGame = (np: number): Game => ({
  players: Array.from({ length: np }, (_, i) => mkPlayer(i)),
  deck: [],
  logs: [],
  id: 'bench',
  name: 'bench',
  status: GAME_STATUS.PLAYING,
  deck_length: 0,
  discard_pile_length: 0,
  flipped: null,
  power_suit: 0,
  first_attacker: 0,
  defender: 0,
  table_battles: [],
  elimination_order: [],
  good_timestamp: null,
  good_players: [],
});

async function main() {
  let actions = 0;
  let legalEvals = 0;
  let finished = 0;

  // Warmup (JIT / wasm instantiation) outside the timed window.
  for (let w = 0; w < 5; w++) {
    const g = mkGame(PLAYERS);
    start_game(g);
    let guard = 0;
    while (game_done(g) === null && ++guard < 100000) {
      const eligible = g.players.filter((p, i) => {
        const ok = shouldBotActCore(g, p, i);
        return ok && calculateLegalMoves(g, p.player_id).length > 0;
      });
      if (eligible.length === 0) break;
      let acted = false;
      for (const p of eligible) {
        if (await processBotAction(g, p)) { acted = true; break; }
      }
      if (!acted) break;
    }
  }

  const t0 = performance.now();
  for (let n = 0; n < GAMES; n++) {
    const g = mkGame(PLAYERS);
    start_game(g);
    let guard = 0;
    let done = false;
    while (game_done(g) === null && ++guard < 100000) {
      const eligible: PrivatePlayer[] = [];
      for (let i = 0; i < g.players.length; i++) {
        const p = g.players[i];
        if (!shouldBotActCore(g, p, i)) continue;
        legalEvals++;
        if (calculateLegalMoves(g, p.player_id).length > 0) eligible.push(p);
      }
      if (eligible.length === 0) break;
      // Deterministic-ish order keeps run-to-run variance about the engine,
      // not the scheduler.
      let acted = false;
      for (const p of eligible) {
        if (await processBotAction(g, p)) { actions++; acted = true; break; }
      }
      if (!acted) break;
    }
    if (game_done(g) !== null) { finished++; done = true; }
    void done;
  }
  const dt = (performance.now() - t0) / 1000;

  out(`engine bench: strategy=${STRATEGY} players=${PLAYERS} games=${GAMES}`);
  out(`  finished:    ${finished}/${GAMES}`);
  out(`  wall:        ${dt.toFixed(2)}s`);
  out(`  games/sec:   ${(GAMES / dt).toFixed(1)}`);
  out(`  actions/sec: ${(actions / dt).toFixed(0)}  (${actions} total)`);
  out(`  legal-evals/sec: ${(legalEvals / dt).toFixed(0)}  (${legalEvals} total)`);
}

main();
