// Stress driver: spins a game on the local-emulated server and hammers it with
// rapid, overlapping calls to the real move handlers through the real CAS +
// fire-and-forget broadcast path — then audits both the durable card state and
// the per-client animation-sequence stream for breakage.
//
//   npx tsx tests/stress/stress.ts [numGames] [--humans=N] [--bots=N] [--delay=ms] [--seed=S]
//
// The goal (per the experiment): see whether animation sequences break under
// load — duplicates, version regressions ("rubber-banding"), or a torn card
// state that the CAS was supposed to make impossible.

import { randomUUID } from 'crypto';
import { Game, AnimationEvent, GAME_STATUS } from '../../supabase/functions/_shared/types.ts';
import { start_game } from '../../supabase/functions/_shared/common_utils.ts';
import { pool, seedGame, resetDb, loadCompleteGame, SeedPlayer, tryAcquireBotLease, releaseBotLease, renewBotLease } from './db.ts';
import { executeWithGameLock, Recorder, Delivery } from './orchestrator.ts';
import { legalMoves, Move } from './moves.ts';
import { applyMove } from './apply.ts';
import { checkCards, analyzeBroadcasts } from './invariants.ts';

const args = process.argv.slice(2);
const flag = (name: string, def: number) => {
  const a = args.find((x) => x.startsWith(`--${name}=`));
  return a ? Number(a.split('=')[1]) : def;
};
const NUM_GAMES = Number(args.find((a) => !a.startsWith('--')) ?? 8);
const HUMANS = flag('humans', 2);
const BOTS = flag('bots', 1);
const COMPUTE_DELAY = flag('delay', 6);  // injected load→commit delay (ms)
const MAX_STEPS = flag('steps', 4000);
// Modelled Supabase Realtime broadcast delivery latency (ms, uniform 0..N). Real
// realtime fan-out is tens-to-hundreds of ms and variable; the production code
// fires broadcasts un-awaited, so this is what decides client arrival order.
const BCAST_LATENCY = flag('blatency', 120);

interface Stats {
  committed: number; conflicts: number; rejected: number; conflictExhausted: number;
  cardViolations: number; firstViolation: string | null;
  broadcastRegressions: number; broadcastDuplicates: number; totalDeliveries: number;
}

const rand = (n: number) => Math.floor(Math.random() * n);
const pick = <T>(arr: T[]): T => arr[rand(arr.length)];

// One "client request": run a move through the full orchestration. Returns a
// classification so we can tell legitimate rejections (stale client view -> 400)
// from CAS contention from real corruption.
type Outcome = 'committed' | 'rejected' | 'conflictExhausted';
async function fireMove(gameId: string, move: Move, recorder: Recorder, reqId: string, stats: Stats): Promise<Outcome> {
  let attempts = 0;
  try {
    const res = await executeWithGameLock(gameId, async (g) => {
      const r = applyMove(g, move);
      return r;
    }, recorder, reqId, true, { computeDelayMs: COMPUTE_DELAY });
    attempts = res.attempts;
    stats.conflicts += attempts - 1;
    return 'committed';
  } catch (e: any) {
    if (String(e.message).includes('write contention')) { stats.conflictExhausted++; return 'conflictExhausted'; }
    // a handler throw = the move was illegal against the committed state the
    // retry reloaded (a stale-client 400). Expected and harmless.
    stats.rejected++;
    return 'rejected';
  }
}

// A faithful-ish replica of lockedBotLoop: lease-guarded worker that drives the
// bot players with random legal moves, renewing its lease each cycle. Runs
// concurrently with the human driver so their broadcasts race.
async function botLoop(gameId: string, botIds: Set<string>, recorder: Recorder, stats: Stats, stop: () => boolean): Promise<void> {
  const TTL = 25_000;
  const token = await tryAcquireBotLease(gameId, TTL);
  if (!token) return;
  try {
    let cycle = 0;
    while (!stop()) {
      if (cycle > 0 && !(await renewBotLease(gameId, token, TTL))) return;
      cycle++;
      const game = await loadCompleteGame(gameId);
      if (game.status !== GAME_STATUS.PLAYING) return;
      const botMoves = legalMoves(game).filter((m) => botIds.has(m.player_id));
      if (botMoves.length === 0) { await sleep(2); continue; }
      const m = pick(botMoves);
      const o = await fireMove(gameId, m, recorder, `bot-${cycle}`, stats);
      if (o === 'committed') stats.committed++;
      await sleep(rand(3));
    }
  } finally {
    await releaseBotLease(gameId, token);
  }
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function auditCards(gameId: string, stats: Stats, where: string): Promise<void> {
  const game = await loadCompleteGame(gameId);
  const chk = checkCards(game);
  if (!chk.ok) {
    stats.cardViolations++;
    if (!stats.firstViolation) stats.firstViolation = `[${where}] ${chk.detail}`;
  }
}

function dupVersionsPerClient(perClient: Map<string, Delivery[]>): number {
  let dups = 0;
  for (const [, deliveries] of perClient) {
    const seen = new Set<number>();
    for (const d of deliveries) {
      if (seen.has(d.committedVersion)) dups++;
      seen.add(d.committedVersion);
    }
  }
  return dups;
}

async function runGame(idx: number, stats: Stats): Promise<void> {
  const gameId = `g${idx}_${randomUUID().slice(0, 4)}`;
  const players: SeedPlayer[] = [];
  for (let i = 0; i < HUMANS; i++) players.push({ id: randomUUID(), name: `Human${i}`, is_ai: false, strategy_key: 'human' });
  for (let i = 0; i < BOTS; i++) players.push({ id: randomUUID(), name: `Bot${i}`, is_ai: true, strategy_key: 'random' });
  const botIds = new Set(players.filter((p) => p.is_ai).map((p) => p.id));

  await seedGame(gameId, players);
  const recorder = new Recorder(BCAST_LATENCY);

  // Start the game (deal) through the lock, like the `start` edge function.
  await executeWithGameLock(gameId, async (g) => {
    const events = start_game(g) as AnimationEvent[];
    return { game: g, events };
  }, recorder, 'start', false, {});
  await auditCards(gameId, stats, 'post-start');

  let stopped = false;
  const stop = () => stopped;
  const bots = BOTS > 0 ? botLoop(gameId, botIds, recorder, stats, stop) : Promise.resolve();

  let steps = 0;
  let idle = 0;
  while (steps < MAX_STEPS) {
    const snapshot = await loadCompleteGame(gameId);
    if (snapshot.status !== GAME_STATUS.PLAYING) break;

    const human = legalMoves(snapshot).filter((m) => !botIds.has(m.player_id));
    if (human.length === 0) {
      idle++;
      if (idle > 200) break; // bots stuck or game wedged
      await sleep(1);
      continue;
    }
    idle = 0;

    // Fire a burst of overlapping human requests to maximise contention:
    //  - pick 1–3 distinct legal moves
    //  - ~30% of the time DOUBLE-SUBMIT one of them (rapid double-click)
    const burst: Move[] = [];
    const n = 1 + rand(Math.min(3, human.length));
    for (let i = 0; i < n; i++) burst.push(pick(human));
    if (Math.random() < 0.3) burst.push(burst[0]); // double-click the same move

    const results = await Promise.all(burst.map((m, i) => fireMove(gameId, m, recorder, `h${steps}-${i}`, stats)));
    for (const r of results) if (r === 'committed') stats.committed++;

    await auditCards(gameId, stats, `step-${steps}`);
    steps++;
  }

  stopped = true;
  await bots;
  await recorder.drain();

  // Final audits.
  await auditCards(gameId, stats, 'final');
  const report = analyzeBroadcasts(recorder.perClient);
  stats.totalDeliveries += report.totalDeliveries;
  stats.broadcastRegressions += report.regressions.length;
  stats.broadcastDuplicates += dupVersionsPerClient(recorder.perClient);
  if (report.regressions.length) {
    const r = report.regressions[0];
    console.log(`  [game ${gameId}] broadcast version REGRESSION: client saw v${r.prevVer} then v${r.gotVer} (${r.reqId})`);
  }
}

async function main(): Promise<void> {
  console.log(`=== Foolish stress harness ===`);
  console.log(`games=${NUM_GAMES} humans=${HUMANS} bots=${BOTS} computeDelay=${COMPUTE_DELAY}ms broadcastLatency=0..${BCAST_LATENCY}ms`);
  await resetDb();
  const stats: Stats = {
    committed: 0, conflicts: 0, rejected: 0, conflictExhausted: 0,
    cardViolations: 0, firstViolation: null,
    broadcastRegressions: 0, broadcastDuplicates: 0, totalDeliveries: 0,
  };

  const t0 = Date.now();
  for (let i = 0; i < NUM_GAMES; i++) {
    process.stdout.write(`running game ${i + 1}/${NUM_GAMES}...\r`);
    await runGame(i, stats);
  }
  const secs = ((Date.now() - t0) / 1000).toFixed(1);

  console.log(`\n=== results (${secs}s) ===`);
  console.log(`committed moves        : ${stats.committed}`);
  console.log(`CAS conflicts (retried): ${stats.conflicts}`);
  console.log(`stale-view rejections  : ${stats.rejected}`);
  console.log(`conflict-exhausted     : ${stats.conflictExhausted}`);
  console.log(`broadcast deliveries   : ${stats.totalDeliveries}`);
  console.log(`--- CORRECTNESS ---`);
  console.log(`card-state violations  : ${stats.cardViolations}${stats.firstViolation ? `  e.g. ${stats.firstViolation}` : ''}`);
  console.log(`broadcast regressions  : ${stats.broadcastRegressions}  (client animates an older state after a newer one)`);
  console.log(`broadcast duplicates   : ${stats.broadcastDuplicates}  (same version delivered twice to one client)`);
  await pool.end();
  if (stats.cardViolations > 0) process.exitCode = 2;
}

main().catch((e) => { console.error(e); process.exit(1); });
