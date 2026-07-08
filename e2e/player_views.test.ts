// player_views — the server-written, client-read personalized view cache
// (docs/PLAYER_VIEWS.md). These prove the WRITE side end-to-end against real
// Postgres running the real commit_game / create_game plpgsql:
//
//  1. A dealt commit writes one MASKED row per human (none for bots), stamped
//     with the committed version + status, byte-identical to what the get_game
//     builder (buildPackedGameBytes) would emit — so the client decodes them
//     with the existing shared decodePackedGame, and each player sees only
//     their own hand.
//  2. create_game seeds the creator's lobby row (no blob → the TS-mirror mask).
//  3. Exiting prunes the leaver's row (they stop seeing the game in their list).
//  4. RLS: a player reads ONLY their own rows; no client can write.

import './harness.ts';
import { test, before, beforeEach } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock, supabaseClient } from '../supabase/functions/_shared/utils.ts';
import { handleMetaAction } from '../supabase/functions/_shared/meta_actions.ts';
import { buildPlayerViewRows } from '../supabase/functions/_shared/player_views.ts';
import { buildPackedGameBytes } from '../supabase/functions/_shared/packed_game.ts';
import { decodePackedGame } from '../supabase/functions/_shared/wire/view.ts';
import { bytesToBareHex } from '../supabase/functions/_shared/wire/bytes.ts';
import { hexToBytes } from '../supabase/functions/_shared/replay/codec.ts';
import {
  Game, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY, PersonalGame,
} from '../supabase/functions/_shared/types.ts';
import { __setKernelSeedSource } from '../supabase/functions/_shared/wasm/engine.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }
__setKernelSeedSource(() => 777);

const GAME_COLS =
  'id,name,status,version,state,players,good_players,good_timestamp';

async function viewsFor(gameId: string): Promise<Map<string, { view: string; version: string; status: string }>> {
  const { rows } = await pgPool.query(
    'SELECT player_id, view, version, status FROM player_views WHERE game_id=$1', [gameId]);
  const m = new Map<string, { view: string; version: string; status: string }>();
  for (const r of rows) m.set(r.player_id, { view: r.view, version: String(r.version), status: r.status });
  return m;
}

before(async () => {
  await applySchema();
  // Make the harness's stub auth.uid() honor a per-connection JWT claim, so the
  // RLS SELECT policy (player_id = auth.uid()) is actually exercised — the
  // default stub returns NULL. Faithful to Supabase's own auth.uid().
  await pgPool.query(`
    CREATE OR REPLACE FUNCTION auth.uid() RETURNS uuid LANGUAGE sql STABLE AS $$
      SELECT NULLIF(current_setting('request.jwt.claims', true)::jsonb->>'sub','')::uuid
    $$;`);
});
beforeEach(async () => { await resetDb(); });

test('dealt commit writes one masked, decodable row per human (not bots)', async () => {
  const gameId = `p${uuid().slice(0, 5)}`;
  const h1 = uuid(), h2 = uuid(), b1 = uuid();
  await seedGame(gameId, [
    { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
    { id: h2, name: 'H2', is_ai: false, strategy_key: 'human' },
    { id: b1, name: '%Bot', is_ai: true, strategy_key: STRATEGY_KEY.RANDOM },
  ]);

  await executeWithGameLock(gameId, async (game) =>
    handleMetaAction({ user: { id: h1 } as any, user_name: 'H1', body: { type: 'start', game_id: gameId }, game, reqId: 'r' }),
    'r', false);

  const g = (await pgPool.query(`SELECT ${GAME_COLS} FROM games WHERE id=$1`, [gameId])).rows[0];
  assert.ok(g.state, 'game dealt (blob present)');

  const views = await viewsFor(gameId);
  assert.equal(views.size, 2, 'one row per human, none for the bot');
  assert.ok(views.has(h1) && views.has(h2), 'both humans have a row');
  assert.ok(!views.has(b1), 'bot has no row');

  for (const pid of [h1, h2]) {
    const row = views.get(pid)!;
    assert.equal(row.status, 'playing', 'status denormalized');
    assert.equal(row.version, String(g.version), 'version mirrors games.version');

    // Byte-identical to the get_game builder's per-viewer envelope: the cache is
    // just that same blob, persisted at write time instead of built per read.
    const expected = bytesToBareHex((await buildPackedGameBytes(g, pid))!);
    assert.equal(row.view, expected, `cached view == get_game view for ${pid}`);

    // Decodes with the shared client codec, and the viewer sees only own hand.
    const decoded = decodePackedGame(hexToBytes(row.view));
    assert.ok(decoded, 'row decodes via decodePackedGame');
    const game = decoded!.game as PersonalGame;
    assert.equal(game.id, gameId);
    assert.equal(game.status, GAME_STATUS.PLAYING);
    assert.equal(game.self.player_id, pid, 'self is the row owner');
    assert.ok(game.self.hand.length > 0, 'owner sees their own hand');
    // The opponent's hand is masked to card-backs (never real cards).
    const opp = game.players.find(p => p.player_id !== pid && !p.is_ai)!;
    assert.ok(opp.hand_length > 0 && !(game as any).players.some((p: any) => p.hand),
      'opponents carry only a count, no cards');
  }
});

test('create_game seeds the creator\'s decodable lobby row', async () => {
  const gameId = `c${uuid().slice(0, 5)}`;
  const h1 = uuid();
  await pgPool.query('INSERT INTO auth.users(id) VALUES($1)', [h1]);

  const lobby: Game = {
    id: gameId, name: 'H1\'s Game', deck: [], deck_length: 0, discard_pile_length: 0,
    flipped: null, status: GAME_STATUS.WAITING, power_suit: 0, first_attacker: 0, defender: 0,
    table_battles: [], elimination_order: [], good_timestamp: null, good_players: [], logs: [],
    players: [{
      player_id: h1, name: 'H1', status: PLAYER_STATUS.IDLE, is_ai: false,
      hand: [], hand_length: 0, awaiting_attack: false, strategy_key: STRATEGY_KEY.HUMAN,
    }],
  };
  const p_views = await buildPlayerViewRows(lobby, null, 0);
  const { error } = await supabaseClient.rpc('create_game', {
    p_game_id: gameId, p_name: lobby.name, p_player_id: h1,
    p_players: [{ player_id: h1, name: 'H1', status: PLAYER_STATUS.IDLE, is_ai: false }],
    p_views,
  });
  assert.ok(!error, `create_game ok: ${error?.message}`);

  const views = await viewsFor(gameId);
  assert.equal(views.size, 1, 'creator row seeded');
  const decoded = decodePackedGame(hexToBytes(views.get(h1)!.view));
  assert.ok(decoded, 'lobby row decodes');
  assert.equal(decoded!.game.status, GAME_STATUS.WAITING);
  assert.equal((decoded!.game as PersonalGame).self.player_id, h1);
});

test('exiting a game prunes the leaver\'s view row', async () => {
  const gameId = `x${uuid().slice(0, 5)}`;
  const h1 = uuid(), h2 = uuid();
  await seedGame(gameId, [
    { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
    { id: h2, name: 'H2', is_ai: false, strategy_key: 'human' },
  ]);

  // A lobby commit (rename) populates a row for BOTH humans.
  await executeWithGameLock(gameId, async (game) =>
    handleMetaAction({ user: { id: h1 } as any, user_name: 'H1', body: { type: 'update-name', game_id: gameId, new_name: 'Renamed' }, game, reqId: 'r' }),
    'r', false);
  let views = await viewsFor(gameId);
  assert.ok(views.has(h1) && views.has(h2), 'both humans have a row after a lobby commit');

  // H2 exits: the next commit's participant set no longer includes them.
  await executeWithGameLock(gameId, async (game) =>
    handleMetaAction({ user: { id: h2 } as any, user_name: 'H2', body: { type: 'exit', game_id: gameId, player_id: h2 }, game, reqId: 'r' }),
    'r', false);
  views = await viewsFor(gameId);
  assert.ok(views.has(h1), 'remaining player keeps their row');
  assert.ok(!views.has(h2), 'the leaver\'s row is pruned');
});

test('RLS: a player reads only their own rows, and clients cannot write', async () => {
  const gameId = `s${uuid().slice(0, 5)}`;
  const h1 = uuid(), h2 = uuid();
  await seedGame(gameId, [
    { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
    { id: h2, name: 'H2', is_ai: false, strategy_key: 'human' },
  ]);
  await executeWithGameLock(gameId, async (game) =>
    handleMetaAction({ user: { id: h1 } as any, user_name: 'H1', body: { type: 'start', game_id: gameId }, game, reqId: 'r' }),
    'r', false);

  const c = await pgPool.connect();
  try {
    // As H1 (authenticated): sees exactly their own row.
    await c.query('BEGIN');
    await c.query('SET LOCAL ROLE authenticated');
    await c.query(`SELECT set_config('request.jwt.claims', json_build_object('sub', $1::text)::text, true)`, [h1]);
    const mine = await c.query('SELECT player_id FROM player_views');
    assert.equal(mine.rows.length, 1, 'H1 sees one row');
    assert.equal(mine.rows[0].player_id, h1, 'and it is their own');

    // A client write is denied (no INSERT grant / policy).
    await assert.rejects(
      c.query(`INSERT INTO player_views(game_id,player_id,view,version,status) VALUES($1,$2,'00',0,'playing')`, [gameId, h1]),
      /permission denied/, 'authenticated cannot insert');
    await c.query('ROLLBACK');
  } finally {
    c.release();
  }
});
