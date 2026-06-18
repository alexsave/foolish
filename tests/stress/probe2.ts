// Round-2 probes: stress the system AFTER the version-gate / authoritative-table /
// reconnect-resync fix, looking for regressions the fix could introduce and fresh
// edge cases. Q1-Q5 are tested here; Q6-Q10 are code-analysis (see FINDINGS_PROBES2.md).
//
//   npx tsx tests/stress/probe2.ts

import { randomUUID } from 'crypto';
import { Game, AnimationEvent, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY } from '../../supabase/functions/_shared/types.ts';
import { start_game } from '../../supabase/functions/_shared/common_utils.ts';
import { handleGood } from '../../supabase/functions/_shared/actions/good.ts';
import { handleAttack } from '../../supabase/functions/_shared/actions/attack.ts';
import { pool, resetDb, seedGame, loadCompleteGame, commitGame, SeedPlayer } from './db.ts';
import { executeWithGameLock, Recorder, checkWinSync } from './orchestrator.ts';
import { legalMoves } from './moves.ts';
import { applyMove } from './apply.ts';

const rec = new Recorder(0);
const rand = (n: number) => Math.floor(Math.random() * n);
const pick = <T>(a: T[]): T => a[rand(a.length)];

// Drive one game to completion single-threaded, recording the committed version of
// every commit that PRODUCES A BROADCAST (events.length > 0) — i.e. exactly the
// sequence of versions a client would receive.
async function playSession(gameId: string, broadcastVersions: number[]): Promise<void> {
  // deal
  const s = await executeWithGameLock(gameId, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), rec, 'start', false, {});
  if (s.events.length > 0) broadcastVersions.push(s.committedVersion);

  let steps = 0;
  while (steps < 4000) {
    const snap = await loadCompleteGame(gameId);
    if (snap.status !== GAME_STATUS.PLAYING) break;
    const moves = legalMoves(snap);
    if (moves.length === 0) break;
    const m = pick(moves);
    try {
      const r = await executeWithGameLock(gameId, async (g) => applyMove(g, m), rec, `s${steps}`, true, {});
      if (r.events.length > 0) broadcastVersions.push(r.committedVersion);
    } catch { /* stale */ }
    steps++;
  }
}

// Replicate the `continue` edge function: reset a finished game back to the lobby
// on the SAME row (version keeps climbing).
async function continueReset(gameId: string): Promise<number | null> {
  const r = await executeWithGameLock(gameId, async (g) => {
    g.status = GAME_STATUS.WAITING;
    g.players.forEach((p) => {
      p.status = p.is_ai ? PLAYER_STATUS.READY : PLAYER_STATUS.IDLE;
      p.hand = []; p.hand_length = 0; p.awaiting_attack = false;
    });
    g.deck = []; g.discard_pile_length = 0; g.flipped = null; g.power_suit = 0;
    g.first_attacker = 0; g.defender = 0; g.table_battles = []; g.elimination_order = [];
    return { game: g, events: [{ type: 'magic_transition', game_state: g } as any] };
  }, rec, 'continue', false, {});
  return r.events.length > 0 ? r.committedVersion : null;
}

// Q1/Q2/Q9-ish: version monotonicity & uniqueness across multiple sessions on one
// row (game -> continue -> game), all humans so checkWin/continue behave.
async function probeVersionOrdering(): Promise<void> {
  console.log('\n=== Q1/Q2/Q3(server): broadcast-version monotonicity & uniqueness across sessions ===');
  let nonMonotone = 0, dupes = 0, runs = 0, crossResetOk = 0, crossResetTotal = 0;
  for (let t = 0; t < 25; t++) {
    await resetDb();
    const gameId = `vo_${randomUUID().slice(0, 5)}`;
    const players: SeedPlayer[] = [
      { id: randomUUID(), name: 'H0', is_ai: false, strategy_key: 'human' },
      { id: randomUUID(), name: 'H1', is_ai: false, strategy_key: 'human' },
    ];
    await seedGame(gameId, players);
    const versions: number[] = [];
    await playSession(gameId, versions);
    const beforeReset = versions[versions.length - 1];
    const resetV = await continueReset(gameId);
    // re-ready the humans + start a new session
    await executeWithGameLock(gameId, async (g) => {
      g.players.forEach((p) => { p.status = PLAYER_STATUS.READY; });
      return { game: g, events: [] };
    }, rec, 'ready', false, {});
    await playSession(gameId, versions);

    runs++;
    for (let i = 1; i < versions.length; i++) {
      if (versions[i] <= versions[i - 1]) nonMonotone++;
    }
    if (versions.length !== new Set(versions).size) dupes++;
    if (resetV != null && beforeReset != null) {
      crossResetTotal++;
      if (resetV > beforeReset) crossResetOk++;
    }
  }
  console.log(`  ${runs} runs, 2 sessions each (game -> continue -> game)`);
  console.log(`Q1 verdict: ${nonMonotone === 0 ? 'no bug — broadcast versions strictly increase' : `BUG: ${nonMonotone} non-monotone steps`}`);
  console.log(`Q2 verdict: ${dupes === 0 ? 'no bug — no duplicate broadcast versions' : `BUG: ${dupes} runs had duplicate versions`}`);
  console.log(`Q3 verdict: ${crossResetOk === crossResetTotal ? 'no bug — version keeps climbing across continue/reset (new deal not gated)' : `BUG: ${crossResetTotal - crossResetOk}/${crossResetTotal} resets did not advance version`}`);
}

// Q4: does an attacker pressing "good" without triggering a transition produce an
// empty event list (no broadcast) — so other clients never see the good live?
function probeSilentGood(): void {
  console.log('\n=== Q4: "silent good" — does a non-transitioning good broadcast nothing? ===');
  // Build a state: 2 attackers + defender, one uncovered attack on the table, no
  // one has said good. An attacker says good — round can't transition (uncovered),
  // so executeGood returns []. good_players still changes server-side.
  const game: Game = {
    id: 'sg', name: 'sg', deck_length: 10, discard_pile_length: 0, flipped: { suit: 0, value: 13 },
    status: GAME_STATUS.PLAYING, power_suit: 0, first_attacker: 0, defender: 1,
    table_battles: [{ attack: { suit: 1, value: 5 }, defense: null }],
    elimination_order: [], good_timestamp: null, good_players: [], deck: [], logs: [],
    players: [
      { player_id: 'A0', name: 'A0', status: PLAYER_STATUS.IN, is_ai: false, hand: [{ suit: 1, value: 6 }], awaiting_attack: true, hand_length: 1, strategy_key: STRATEGY_KEY.HUMAN },
      { player_id: 'D', name: 'D', status: PLAYER_STATUS.IN, is_ai: false, hand: [{ suit: 0, value: 6 }], awaiting_attack: false, hand_length: 1, strategy_key: STRATEGY_KEY.HUMAN },
      { player_id: 'A1', name: 'A1', status: PLAYER_STATUS.IN, is_ai: false, hand: [{ suit: 2, value: 7 }], awaiting_attack: true, hand_length: 1, strategy_key: STRATEGY_KEY.HUMAN },
    ],
  };
  const before = [...game.good_players];
  const events = handleGood(game, 'A0');
  const goodChanged = JSON.stringify(before) !== JSON.stringify(game.good_players);
  const silent = events.length === 0;
  console.log(`  good_players ${before} -> ${game.good_players}; events emitted: ${events.length}`);
  console.log(`Q4 verdict: ${silent && goodChanged
    ? 'CONFIRMED (pre-existing): a non-transitioning good changes server state but emits NO broadcast — other clients do not see the good indicator until the round transitions or another broadcast arrives.'
    : 'not reproduced (good emitted events)'}`);
}

// Q5: within one accepted sequence, is each event's table a forward step, so
// authoritative replace-per-event animates correctly (never drops a card a later
// event re-adds)? We check across many real sequences that no card present in an
// earlier event of a sequence disappears and then reappears later in the SAME
// sequence (which replace would render as a flicker).
async function probeIntraSequenceForward(): Promise<void> {
  console.log('\n=== Q5: intra-sequence table steps monotone (safe for authoritative replace)? ===');
  let flickers = 0, sequences = 0, gamesRun = 0;
  for (let t = 0; t < 20; t++) {
    await resetDb();
    const gameId = `is_${randomUUID().slice(0, 5)}`;
    await seedGame(gameId, [
      { id: randomUUID(), name: 'H', is_ai: false, strategy_key: 'human' },
      { id: randomUUID(), name: 'B', is_ai: true, strategy_key: 'random' },
      { id: randomUUID(), name: 'B2', is_ai: true, strategy_key: 'random' },
    ]);
    gamesRun++;
    const collect = (events: AnimationEvent[]) => {
      sequences++;
      const tables = events.map((e) => (e.game_state?.table_battles ?? []).map((b: any) => `${b.attack.suit}:${b.attack.value}`));
      // card flicker within a sequence: a card key present in event i, absent in i+1, present again later
      const everPresent = new Set<string>();
      const seenGone = new Set<string>();
      for (const t2 of tables) {
        const now = new Set(t2);
        for (const k of everPresent) if (!now.has(k)) seenGone.add(k);
        for (const k of now) { if (seenGone.has(k)) flickers++; everPresent.add(k); }
      }
    };
    const s = await executeWithGameLock(gameId, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), rec, 'start', false, {});
    collect(s.events);
    let steps = 0;
    while (steps < 1500) {
      const snap = await loadCompleteGame(gameId);
      if (snap.status !== GAME_STATUS.PLAYING) break;
      const moves = legalMoves(snap);
      if (moves.length === 0) break;
      try { const r = await executeWithGameLock(gameId, async (g) => applyMove(g, pick(moves)), rec, `s${steps}`, true, {}); collect(r.events); } catch { /* */ }
      steps++;
    }
  }
  console.log(`  ${gamesRun} games, ${sequences} sequences inspected`);
  console.log(`Q5 verdict: ${flickers === 0 ? 'no bug — within a sequence the table only moves forward; per-event authoritative replace is safe' : `WARN: ${flickers} intra-sequence card flickers (replace could show a momentary drop)`}`);
}

async function main(): Promise<void> {
  console.log('=== Foolish round-2 probes (post-fix) ===');
  await resetDb();
  await probeVersionOrdering();
  probeSilentGood();
  await probeIntraSequenceForward();
  await pool.end();
}
main().catch((e) => { console.error(e); process.exit(1); });
