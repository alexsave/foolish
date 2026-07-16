// Resilience of the server's optimistic-concurrency core (executeWithGameLock +
// the version-gated commit_game RPC) against the failure modes it is built to
// survive: a concurrent writer committing under us, sustained write contention,
// a vanished game, a move landing on an already-finished game, and the
// best-effort end-of-game side effects (ELO + replay snapshot) actually landing.
//
// Nothing here mocks the database — it is the real commit_game plpgsql and the
// real loader running in Postgres, with contention injected by a second writer.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import {
  executeWithGameLock, loadCompleteGame, commitGame,
} from '../supabase/functions/_shared/adapter/utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import { GAME_STATUS, AnimationEvent } from '../supabase/functions/_shared/types.ts';
import { legalMovesFor, applyPlayerMove } from './dispatch.ts';

const pick = <T>(a: T[]): T => a[Math.floor(Math.random() * a.length)];
const dbVersion = async (id: string): Promise<number> =>
  Number((await pgPool.query('SELECT version FROM games WHERE id=$1', [id])).rows[0].version);
const bumpVersion = (id: string) => pgPool.query('UPDATE games SET version = version + 1 WHERE id=$1', [id]);

// Seed + start a 2-bot game (real deal via the kernel), leaving it PLAYING.
async function startedGame(): Promise<string> {
  const id = `rs${uuid().slice(0, 6)}`;
  await seedGame(id, [
    { id: uuid(), name: 'B0', is_ai: true, strategy_key: 'random' },
    { id: uuid(), name: 'B1', is_ai: true, strategy_key: 'random' },
  ]);
  await executeWithGameLock(id, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);
  return id;
}

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });
after(async () => { await pgPool.end(); });

test('commit_game fences a stale version and accepts the fresh one', async () => {
  const id = await startedGame();
  const g = await loadCompleteGame(id);
  const staleVersion = g.version!;

  // A concurrent writer commits first (version moves on).
  await bumpVersion(id);

  const conflict = await commitGame(g, staleVersion);
  assert.equal(conflict.status, 'conflict', 'a stale-version commit is rejected');

  // Reload the now-current version and commit cleanly.
  const g2 = await loadCompleteGame(id);
  const freshVersion = Number(g2.version!);   // BIGINT arrives as a string via node-pg
  const ok = await commitGame(g2, freshVersion);
  assert.equal(ok.status, 'ok', 'a fresh-version commit succeeds');
  assert.equal(Number(ok.version), freshVersion + 1, 'the fence bumps the version by one');
});

test('executeWithGameLock recovers from a concurrent write (conflict -> reload -> retry -> commit)', async () => {
  const id = await startedGame();
  const startVersion = await dbVersion(id);

  let calls = 0;
  await executeWithGameLock(id, async (g) => {
    calls++;
    // On the first pass only, simulate another actor committing between our
    // load and our commit — that pass MUST conflict and be redone.
    if (calls === 1) await bumpVersion(id);
    return { game: g, events: [] };
  }, 'recover', false);

  assert.ok(calls >= 2, `operation was retried after the conflict (ran ${calls}x)`);
  // startVersion +1 (the injected writer) +1 (our eventual successful commit).
  assert.equal(await dbVersion(id), startVersion + 2, 'exactly the injected write + our redone commit landed');
});

test('executeWithGameLock gives up cleanly under sustained contention (bounded, no hang)', async () => {
  const id = await startedGame();

  let calls = 0;
  await assert.rejects(
    executeWithGameLock(id, async (g) => {
      calls++;
      await bumpVersion(id);   // every attempt is stomped -> every commit conflicts
      return { game: g, events: [] };
    }, 'contention', false),
    /write contention/i,
    'exhausting the retries surfaces a clean error, not a hang',
  );
  assert.equal(calls, 5, 'bounded at exactly MAX_ATTEMPTS (5), never an unbounded spin');
});

test('loadCompleteGame throws a clean "not found" for a missing game', async () => {
  await assert.rejects(loadCompleteGame(`ghost-${uuid().slice(0, 6)}`), /not found/i);
});

test('a move on an already-finished game is a moot no-op, never a crash', async () => {
  const id = await startedGame();
  await pgPool.query(`UPDATE games SET status='game_over' WHERE id=$1`, [id]);

  let operationRan = false;
  const res = await executeWithGameLock(id, async (g) => {
    operationRan = true;                      // must NOT run for a finished game
    throw new Error('handler should never be invoked on a finished game');
  }, 'moot', /*mootIfGameOver*/ true);

  assert.equal(operationRan, false, 'the handler is short-circuited');
  assert.equal(res.events.length, 0, 'no events emitted');
  assert.equal(res.game.status, GAME_STATUS.GAME_OVER, 'the finished state is returned as-is');
});

test('a full game commits GAME_OVER and lands its end-of-game side effects (ELO + snapshot)', async () => {
  const id = await startedGame();

  for (let step = 0; step < 300; step++) {
    const g = await loadCompleteGame(id);
    if (g.status !== GAME_STATUS.PLAYING) break;
    const moves = legalMovesFor(g);
    if (moves.length === 0) break;
    try {
      await executeWithGameLock(id, async (gg) => ({ game: gg, events: applyPlayerMove(gg, pick(moves)) }), `p${step}`, true);
    } catch { /* transient contention — not expected single-threaded, but harmless */ }
  }

  const finalStatus = (await pgPool.query('SELECT status FROM games WHERE id=$1', [id])).rows[0].status;
  assert.equal(finalStatus, 'game_over', 'the game reached and durably committed GAME_OVER');

  // The replay snapshot is a best-effort side effect — but on the happy path it
  // must land (proving finalizeEndedGame ran to completion).
  const snaps = Number((await pgPool.query('SELECT count(*)::int AS n FROM game_snapshots WHERE game_id=$1', [id])).rows[0].n);
  assert.equal(snaps, 1, 'exactly one replay snapshot row was written');

  // updateEloRatings ran: the two bots no longer sit at the 1000 default.
  const elos = (await pgPool.query('SELECT elo_rating, games_played FROM bots')).rows;
  assert.ok(elos.some((r: any) => r.elo_rating !== 1000), 'at least one bot rating moved off the default');
  assert.ok(elos.every((r: any) => r.games_played === 1), 'every bot recorded exactly one played game');
});
