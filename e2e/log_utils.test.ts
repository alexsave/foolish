// log_utils.ts is pure DB plumbing (session-log loading + retention cleanup)
// that the per-move e2e flow never triggers: logs now load lazily and the
// cleanup/wipe paths only run at game end. This drives all three exports
// against real Postgres via the pg-backed supabase adapter.
//
// Needs Postgres (the harness applies the real schema).

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { createClient } from './adapters/supabase.ts';
import {
  loadCurrentSessionLogs, wipeAllGameLogs, cleanupOldGameLogs,
} from '../supabase/functions/_shared/log_utils.ts';
import { LOG_TYPE } from '../supabase/functions/_shared/types.ts';

const client = createClient() as any;

// Insert one game_logs row, optionally aged into the past.
async function addLog(gameId: string, type: string, agoDays = 0): Promise<void> {
  await pgPool.query(
    `INSERT INTO game_logs(game_id, log_type, player_id, card_pairs, defender_index, created_at)
     VALUES($1, $2::log_type, NULL, '[]'::jsonb, NULL, NOW() - ($3 || ' days')::interval)`,
    [gameId, type, agoDays],
  );
}

const seatCount = async (gameId: string): Promise<number> =>
  Number((await pgPool.query('SELECT count(*)::int AS n FROM game_logs WHERE game_id=$1', [gameId])).rows[0].n);

async function freshGame(): Promise<string> {
  const id = `lg${uuid().slice(0, 6)}`;
  await seedGame(id, [{ id: uuid(), name: 'B', is_ai: true, strategy_key: 'random' }]);
  return id;
}

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });
after(async () => { await pgPool.end(); });

test('loadCurrentSessionLogs returns only the logs after the most recent GAME_START', async () => {
  const id = await freshGame();
  // An old session, then a fresh one — only the fresh session should come back.
  await addLog(id, LOG_TYPE.GAME_START);
  await addLog(id, LOG_TYPE.ATTACK);
  await addLog(id, LOG_TYPE.GAME_START);   // new session boundary
  await addLog(id, LOG_TYPE.ATTACK);
  await addLog(id, LOG_TYPE.GOOD);

  const logs = await loadCurrentSessionLogs(client, id);
  assert.equal(logs.length, 3, 'GAME_START + the two logs after it');
  assert.equal(logs[0].log_type, LOG_TYPE.GAME_START, 'session begins at the GAME_START');
});

test('loadCurrentSessionLogs returns [] with no GAME_START, and [] when empty', async () => {
  const id = await freshGame();
  await addLog(id, LOG_TYPE.ATTACK);   // logs but no GAME_START marker
  assert.deepEqual(await loadCurrentSessionLogs(client, id), [], 'no GAME_START -> empty');

  const empty = await freshGame();
  assert.deepEqual(await loadCurrentSessionLogs(client, empty), [], 'no rows -> empty');
});

test('wipeAllGameLogs deletes every log row for the game', async () => {
  const id = await freshGame();
  await addLog(id, LOG_TYPE.GAME_START);
  await addLog(id, LOG_TYPE.ATTACK);
  assert.equal(await seatCount(id), 2, 'seeded two logs');
  await wipeAllGameLogs(client, id);
  assert.equal(await seatCount(id), 0, 'all logs wiped');
});

test('cleanupOldGameLogs removes logs older than two weeks, keeps the current session', async () => {
  const id = await freshGame();
  await addLog(id, LOG_TYPE.GAME_START, 30);  // month-old prior session
  await addLog(id, LOG_TYPE.ATTACK, 30);
  await addLog(id, LOG_TYPE.GAME_START, 0);   // current session start
  await addLog(id, LOG_TYPE.ATTACK, 0);
  assert.equal(await seatCount(id), 4, 'seeded four logs');

  await cleanupOldGameLogs(client, id);
  assert.equal(await seatCount(id), 2, 'the two month-old logs were pruned');
});

test('cleanupOldGameLogs is a safe no-op when the game has no GAME_START', async () => {
  const id = await freshGame();
  await addLog(id, LOG_TYPE.ATTACK, 30);   // old, but no session marker
  await cleanupOldGameLogs(client, id);
  assert.equal(await seatCount(id), 1, 'nothing pruned without a GAME_START anchor');
});
