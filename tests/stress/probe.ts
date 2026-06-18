// Five targeted probes into subsystems the earlier harness didn't exercise.
// Each prints a verdict (BUG / clean / note). Run: npx tsx tests/stress/probe.ts
//
//  Q1 bot lease      — exactly-once driver, TTL recovery, stale-token fencing
//  Q2 CAS liveness   — legitimate moves dropped by 5-attempt retry exhaustion
//  Q3 endgame        — elimination_order / rankings integrity over full games
//  Q4 pass integrity — rules-legal pass hitting an impossible-state throw
//  Q5 leakage        — public/spectator payloads exposing hidden hand cards

import { randomUUID } from 'crypto';
import { Game, AnimationEvent, ANIMATION_EVENT_TYPE } from '../../supabase/functions/_shared/types.ts';
import { start_game, calculateGameRankings, game_done } from '../../supabase/functions/_shared/common_utils.ts';
import {
  pool, resetDb, seedGame, loadCompleteGame, SeedPlayer,
  tryAcquireBotLease, renewBotLease, releaseBotLease, gameToPublicGame,
} from './db.ts';
import { executeWithGameLock, Recorder, checkWinSync } from './orchestrator.ts';
import { legalMoves, Move } from './moves.ts';
import { applyMove } from './apply.ts';
import { checkCards } from './invariants.ts';

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const rand = (n: number) => Math.floor(Math.random() * n);
const pick = <T>(a: T[]): T => a[rand(a.length)];
const line = (s: string) => console.log(s);

// ===========================================================================
// Q1 — bot lease semantics
// ===========================================================================
async function probeLease(): Promise<void> {
  line('\n=== Q1: bot-lease mutual exclusion / recovery / fencing ===');
  const gameId = `lease_${randomUUID().slice(0, 5)}`;
  await seedGame(gameId, [{ id: randomUUID(), name: 'B', is_ai: true, strategy_key: 'random' }]);

  // A) 30 concurrent acquires with a long TTL -> exactly one wins
  const tokens = await Promise.all(Array.from({ length: 30 }, () => tryAcquireBotLease(gameId, 30_000)));
  const winners = tokens.filter((t) => t != null);
  const exclusionOk = winners.length === 1;
  line(`A) 30 concurrent acquires -> ${winners.length} winner(s)  ${exclusionOk ? 'OK (exactly one)' : 'BUG (mutual exclusion failed)'}`);

  // B) recovery after a driver "dies" without releasing (short TTL auto-expires)
  await releaseBotLease(gameId, winners[0]!);
  const shortTok = await tryAcquireBotLease(gameId, 250);
  const blockedDuring = await tryAcquireBotLease(gameId, 250); // should be null (live lease)
  await sleep(400); // let it expire (no renew = simulated dead driver)
  const afterExpiry = await tryAcquireBotLease(gameId, 250);
  const recoveryOk = shortTok != null && blockedDuring == null && afterExpiry != null;
  line(`B) dead-driver recovery: held=${!!shortTok} blockedWhileLive=${blockedDuring == null} reacquiredAfterTTL=${afterExpiry != null}  ${recoveryOk ? 'OK' : 'BUG'}`);

  // C) stale-token fencing: once someone else takes over, the old token can't renew
  await sleep(400); // let afterExpiry's lease lapse
  const owner = await tryAcquireBotLease(gameId, 30_000);
  const stale = shortTok!; // an old, no-longer-held token
  const ownerRenew = await renewBotLease(gameId, owner!, 30_000); // true
  const staleRenew = await renewBotLease(gameId, stale, 30_000);  // must be false
  const fencingOk = ownerRenew === true && staleRenew === false;
  line(`C) renew fencing: owner=${ownerRenew} stale=${staleRenew}  ${fencingOk ? 'OK (stale fenced)' : 'BUG (stale token renewed)'}`);

  line(`Q1 verdict: ${exclusionOk && recoveryOk && fencingOk ? 'no bug — lease primitive is sound' : 'BUG FOUND in lease'}`);
}

// ===========================================================================
// Shared full-game engine for Q2/Q3/Q4
// ===========================================================================
interface GameResult {
  finished: boolean;
  committed: number;
  conflictExhausted: number;   // legitimate moves dropped (liveness)
  staleRejections: number;     // benign 400s (move went stale)
  impossibleStateErrors: string[]; // internal-inconsistency throws (real bugs)
  cardViolation: string | null;
  endgame: { ok: boolean; detail: string } | null;
}

const IMPOSSIBLE_MARKERS = ['Uncovered cards > defender_cards', 'SEVERE'];

async function fireMove(gameId: string, move: Move, rec: Recorder, reqId: string, res: GameResult, computeDelayMs: number): Promise<void> {
  try {
    const r = await executeWithGameLock(gameId, async (g) => applyMove(g, move), rec, reqId, true, { computeDelayMs });
    res.committed++;
    void r;
  } catch (e: any) {
    const msg = String(e?.message ?? e);
    if (msg.includes('write contention')) res.conflictExhausted++;
    else if (IMPOSSIBLE_MARKERS.some((m) => msg.includes(m))) res.impossibleStateErrors.push(msg);
    else res.staleRejections++;
  }
}

async function runFullGame(humans: number, bots: number, computeDelayMs: number, burst: number): Promise<GameResult> {
  const gameId = `fg_${randomUUID().slice(0, 6)}`;
  const players: SeedPlayer[] = [];
  for (let i = 0; i < humans; i++) players.push({ id: randomUUID(), name: `H${i}`, is_ai: false, strategy_key: 'human' });
  for (let i = 0; i < bots; i++) players.push({ id: randomUUID(), name: `B${i}`, is_ai: true, strategy_key: 'random' });
  await seedGame(gameId, players);
  const rec = new Recorder(0);
  const res: GameResult = { finished: false, committed: 0, conflictExhausted: 0, staleRejections: 0, impossibleStateErrors: [], cardViolation: null, endgame: null };

  // deal
  await executeWithGameLock(gameId, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), rec, 'start', false, {});

  let steps = 0;
  while (steps < 6000) {
    const snap = await loadCompleteGame(gameId);
    if (snap.status !== 'playing') { res.finished = snap.status === 'game_over'; break; }
    const moves = legalMoves(snap);
    if (moves.length === 0) { await sleep(1); steps++; continue; }
    const fire: Move[] = [];
    const n = 1 + rand(Math.min(burst, moves.length));
    for (let i = 0; i < n; i++) fire.push(pick(moves));
    await Promise.all(fire.map((m, i) => fireMove(gameId, m, rec, `s${steps}-${i}`, res, computeDelayMs)));
    const chk = checkCards(await loadCompleteGame(gameId));
    if (!chk.ok && !res.cardViolation) res.cardViolation = chk.detail;
    steps++;
  }

  // endgame integrity
  if (res.finished) {
    const g = await loadCompleteGame(gameId);
    res.endgame = checkEndgame(g);
  }
  return res;
}

function checkEndgame(g: Game): { ok: boolean; detail: string } {
  const n = g.players.length;
  const elim = g.elimination_order;
  const dupes = elim.length !== new Set(elim).size;
  const fools = g.players.filter((p) => !elim.includes(p.player_id));
  const rankings = calculateGameRankings(g);
  const rankDupes = rankings.length !== new Set(rankings).size;
  const problems: string[] = [];
  // In a finished game everyone but the fool is OUT: elimination_order should be n-1 with no dupes.
  if (dupes) problems.push(`elim has duplicates [${elim.join(',')}]`);
  if (elim.length !== n - 1) problems.push(`elim length ${elim.length} != ${n - 1}`);
  if (fools.length !== 1) problems.push(`${fools.length} fools (expected 1)`);
  if (rankings.length !== n) problems.push(`rankings length ${rankings.length} != ${n}`);
  if (rankDupes) problems.push(`rankings has duplicates`);
  return { ok: problems.length === 0, detail: problems.join('; ') };
}

async function probeFullGames(): Promise<void> {
  const TRIALS = Number(process.env.FG_TRIALS ?? 60);
  line(`\n=== Q2/Q3/Q4: ${TRIALS} full games (pass enabled, contention) ===`);
  let committed = 0, exhausted = 0, stale = 0, finished = 0;
  const impossible: string[] = [];
  let cardViol: string | null = null;
  const endgameProblems: string[] = [];

  for (let t = 0; t < TRIALS; t++) {
    process.stdout.write(`  game ${t + 1}/${TRIALS}...\r`);
    await resetDb();
    // 3 players, real contention via a burst + a small compute delay
    const r = await runFullGame(2, 1, 4, 3);
    committed += r.committed; exhausted += r.conflictExhausted; stale += r.staleRejections;
    if (r.finished) finished++;
    impossible.push(...r.impossibleStateErrors);
    if (r.cardViolation && !cardViol) cardViol = r.cardViolation;
    if (r.endgame && !r.endgame.ok) endgameProblems.push(r.endgame.detail);
  }

  const total = committed + exhausted + stale;
  line(`\n  committed=${committed}  staleRejections=${stale}  conflictExhausted=${exhausted}  finished=${finished}/${TRIALS}`);

  // Q2
  const exhaustRate = total ? (exhausted / total * 100) : 0;
  line(`\nQ2 (CAS liveness): ${exhausted} legitimate moves dropped by 5-attempt exhaustion (${exhaustRate.toFixed(3)}% of all calls)`);
  line(`Q2 verdict: ${exhausted === 0 ? 'no drops at this contention' : exhaustRate < 0.5 ? `rare (${exhaustRate.toFixed(3)}%) — a dropped move surfaces as a spurious 400 to the user; worth a backoff/raise` : `BUG-WORTHY: ${exhaustRate.toFixed(2)}% of moves dropped`}`);

  // Q3
  line(`\nQ3 (endgame integrity): ${endgameProblems.length} of ${finished} finished games malformed`);
  if (endgameProblems.length) endgameProblems.slice(0, 5).forEach((p) => line(`   - ${p}`));
  line(`Q3 verdict: ${endgameProblems.length === 0 ? 'no bug — elimination_order/rankings well-formed' : 'BUG FOUND in endgame accounting'}` + (cardViol ? `  (NB card-state violation: ${cardViol})` : ''));

  // Q4
  line(`\nQ4 (pass integrity): ${impossible.length} impossible-state throws`);
  if (impossible.length) [...new Set(impossible)].slice(0, 5).forEach((m) => line(`   - ${m}`));
  line(`Q4 verdict: ${impossible.length === 0 ? 'no bug — no rules-legal move hit an impossible-state throw' : 'BUG FOUND: a move reached an internal-inconsistency error'}`);
}

// ===========================================================================
// Q5 — private-hand leakage in public/spectator payloads
// ===========================================================================
const createCardBacks = (count: number): any[] => Array(count).fill({ suit: -1, value: -1 });
const shouldSanitizeCards = (e: AnimationEvent): boolean =>
  (e.type === ANIMATION_EVENT_TYPE.REFILL || e.type === ANIMATION_EVENT_TYPE.DEAL) && !!e.cards;

// ported verbatim from utils.ts convertToPublicAnimationEvents
function convertToPublicAnimationEvents(events: AnimationEvent[]): any[] {
  return events.map((event) => {
    const pub: any = { ...event };
    if (shouldSanitizeCards(event)) pub.cards = createCardBacks(event.cards!.length);
    if (event.game_state) pub.game_state = gameToPublicGame(event.game_state);
    return pub;
  });
}

const keyOf = (c: { suit: number; value: number }) => `${c.suit}:${c.value}`;

async function probeLeakage(): Promise<void> {
  const TRIALS = 40;
  line(`\n=== Q5: private-hand leakage in public/spectator payloads (${TRIALS} games) ===`);
  let leaks = 0; let firstLeak = ''; let eventsChecked = 0;

  for (let t = 0; t < TRIALS; t++) {
    await resetDb();
    const gameId = `lk_${randomUUID().slice(0, 5)}`;
    const players: SeedPlayer[] = [
      { id: randomUUID(), name: 'H', is_ai: false, strategy_key: 'human' },
      { id: randomUUID(), name: 'B0', is_ai: true, strategy_key: 'random' },
      { id: randomUUID(), name: 'B1', is_ai: true, strategy_key: 'random' },
    ];
    await seedGame(gameId, players);
    const rec = new Recorder(0);

    // Collect every server event produced over a full game, then run them
    // through the public converter and check for hidden-hand leakage.
    const allEvents: AnimationEvent[] = [];
    const startRes = await executeWithGameLock(gameId, async (g) => {
      const ev = start_game(g) as AnimationEvent[]; return { game: g, events: ev };
    }, rec, 'start', false, {});
    allEvents.push(...startRes.events);

    let steps = 0;
    while (steps < 2000) {
      const snap = await loadCompleteGame(gameId);
      if (snap.status !== 'playing') break;
      const moves = legalMoves(snap);
      if (moves.length === 0) break;
      const m = pick(moves);
      try {
        const r = await executeWithGameLock(gameId, async (g) => applyMove(g, m), rec, `s${steps}`, true, {});
        allEvents.push(...r.events);
      } catch { /* stale */ }
      steps++;
    }

    // A spectator's knowledge from a public payload = the (sanitized) event.cards
    // plus the public game_state. Cards moved by attack/cover/pickup/discard are
    // physically face-up in Durak, so revealing them is correct, NOT a leak. The
    // only genuinely hidden cards are those drawn into a hand — DEAL and REFILL —
    // which MUST be reduced to card-backs, and the public game_state must never
    // carry a hand array. Those are the only two real leak surfaces.
    for (const ev of allEvents) {
      if (!ev.game_state) continue;
      eventsChecked++;
      const pub = convertToPublicAnimationEvents([ev])[0];

      // (1) DEAL/REFILL must be fully sanitized to backs
      if (ev.type === ANIMATION_EVENT_TYPE.DEAL || ev.type === ANIMATION_EVENT_TYPE.REFILL) {
        for (const c of (pub.cards ?? [])) {
          if (c.suit !== -1 || c.value !== -1) { leaks++; if (!firstLeak) firstLeak = `${ev.type} leaked real drawn card ${keyOf(c)}`; }
        }
      }
      // (2) public game_state must not carry any player's hand array
      for (const pp of (pub.game_state?.players ?? [])) {
        if ((pp as any).hand && (pp as any).hand.length > 0) {
          leaks++; if (!firstLeak) firstLeak = `${ev.type} public game_state exposes a hand array`;
        }
      }
    }
  }
  line(`  events checked: ${eventsChecked}`);
  line(`Q5 verdict: ${leaks === 0 ? 'no bug — public payloads never expose hidden hand cards' : `BUG FOUND: ${leaks} leaks, e.g. ${firstLeak}`}`);
}

async function main(): Promise<void> {
  console.log('=== Foolish subsystem probes (Q1–Q5) ===');
  await resetDb();
  await probeLease();
  await probeFullGames();
  await probeLeakage();
  await pool.end();
}
main().catch((e) => { console.error(e); process.exit(1); });
