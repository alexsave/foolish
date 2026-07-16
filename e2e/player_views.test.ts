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
//
// And the sibling spectator_views cache (the SHARED, fully-masked seat -1 view
// that replaced get_game's spectate path):
//  5. A dealt commit / create_game writes one fully-masked (no self, no hands)
//     spectator row, decodable by the same client codec.
//  6. RLS: ANY authenticated user may read it (it carries no hidden state); no
//     client can write.

import './harness.ts';
import { test, before, beforeEach } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock, loadCompleteGame, supabaseClient } from '../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { handleMetaAction } from '../server/impls/supabase/functions/_shared/adapter/meta_actions.ts';
import { legalMovesFor, applyPlayerMove } from './dispatch.ts';
import { buildPlayerViewRows, buildPlayerViewUpserts, buildSpectatorView } from '../server/api/common/player_views.ts';
import { buildPackedGameBytes, gameViewFromRow } from '../server/api/common/packed_game.ts';
import { decodePackedGame } from '../sdk/ts/wire/view.ts';
import { bytesToBareHex } from '../sdk/ts/wire/bytes.ts';
import { hexToBytes } from '../server/api/common/replay/codec.ts';
import {
  Game, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY, PersonalGame,
} from '../server/api/core/types.ts';
import { __setKernelSeedSource } from '../sdk/ts/wasm/engine.ts';

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

async function spectatorFor(gameId: string): Promise<{ view: string; version: string; status: string } | null> {
  const { rows } = await pgPool.query(
    'SELECT view, version, status FROM spectator_views WHERE game_id=$1', [gameId]);
  if (rows.length === 0) return null;
  return { view: rows[0].view, version: String(rows[0].version), status: rows[0].status };
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
  // Likewise honor a per-connection role claim so the spectator_views policy
  // (auth.role() = 'authenticated') is actually exercised. Falls back to the
  // default stub value ('service_role') when no claim is set, so every other
  // test that relies on the service-role bypass is unaffected. Faithful to
  // Supabase's own auth.role().
  await pgPool.query(`
    CREATE OR REPLACE FUNCTION auth.role() RETURNS text LANGUAGE sql STABLE AS $$
      SELECT COALESCE(NULLIF(current_setting('request.jwt.claims', true)::jsonb->>'role',''), 'service_role')
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

test('a move refreshes each human\'s cached view (version bumped, still masked & decodable)', async () => {
  const gameId = `m${uuid().slice(0, 5)}`;
  const h1 = uuid(), h2 = uuid();
  await seedGame(gameId, [
    { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
    { id: h2, name: 'H2', is_ai: false, strategy_key: 'human' },
  ]);
  await executeWithGameLock(gameId, async (game) =>
    handleMetaAction({ user: { id: h1 } as any, user_name: 'H1', body: { type: 'start', game_id: gameId }, game, reqId: 'r' }),
    'r', false);
  const before = await viewsFor(gameId);

  // Apply one real legal move (the same enumeration + dispatch the bot loop
  // uses) through executeWithGameLock → commitGame, the live move commit path.
  await executeWithGameLock(gameId, async (game) => {
    const pms = legalMovesFor(game);
    const events = pms.length ? applyPlayerMove(game, pms[0]) : [];
    return { game, events };
  }, 'r', true);

  const g = (await pgPool.query(`SELECT ${GAME_COLS} FROM games WHERE id=$1`, [gameId])).rows[0];
  const after = await viewsFor(gameId);
  assert.equal(after.size, 2, 'still one row per human after the move');

  for (const pid of [h1, h2]) {
    assert.ok(Number(after.get(pid)!.version) > Number(before.get(pid)!.version),
      `version bumped for ${pid}`);
    assert.equal(after.get(pid)!.version, String(g.version), 'and mirrors the new games.version');
    // Still byte-identical to the get_game builder on the NEW state.
    assert.equal(after.get(pid)!.view, bytesToBareHex((await buildPackedGameBytes(g, pid))!),
      `refreshed cache == get_game view for ${pid}`);
    const game = decodePackedGame(hexToBytes(after.get(pid)!.view))!.game as PersonalGame;
    assert.equal(game.self.player_id, pid, 'self still the owner');
    assert.ok(!game.players.some((p: any) => p.player_id !== pid && p.hand),
      'opponents still carry no cards');
  }
});

test('backfill (buildPlayerViewUpserts) rebuilds ALL participants byte-identically, fill-if-absent never overwrites', async () => {
  const gameId = `b${uuid().slice(0, 5)}`;
  const h1 = uuid(), h2 = uuid(), b1 = uuid();
  await seedGame(gameId, [
    { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
    { id: h2, name: 'H2', is_ai: false, strategy_key: 'human' },
    { id: b1, name: '%Bot', is_ai: true, strategy_key: STRATEGY_KEY.RANDOM },
  ]);
  await executeWithGameLock(gameId, async (game) =>
    handleMetaAction({ user: { id: h1 } as any, user_name: 'H1', body: { type: 'start', game_id: gameId }, game, reqId: 'r' }),
    'r', false);

  // The exact games row get_game hands to gameViewFromRow.
  const cols = 'id,name,status,version,state,players,good_players,good_timestamp,' +
    'discard_pile_length,flipped,power_suit,first_attacker,defender,table_battles,elimination_order';
  const row = (await pgPool.query(`SELECT ${cols} FROM games WHERE id=$1`, [gameId])).rows[0];
  const committed = await viewsFor(gameId);

  // One load builds a row for EVERY human (not just the invoker), none for bots.
  const ups = await buildPlayerViewUpserts(gameViewFromRow(row), row.state ?? null, Number(row.version));
  const byId = new Map(ups.map(u => [u.player_id, u]));
  assert.deepEqual([...byId.keys()].sort(), [h1, h2].sort(), 'a row for each human, none for the bot');
  for (const pid of [h1, h2]) {
    assert.equal(byId.get(pid)!.view, committed.get(pid)!.view, `backfilled view == committed view (${pid})`);
    assert.equal(String(byId.get(pid)!.version), committed.get(pid)!.version, 'same version');
  }

  // (a) Simulate a game predating the cache: drop ALL its rows, then restore via
  // the fill-if-absent insert get_game does.
  await pgPool.query('DELETE FROM player_views WHERE game_id=$1', [gameId]);
  for (const u of ups) {
    await pgPool.query(
      `INSERT INTO player_views(game_id,player_id,view,version,status) VALUES($1,$2,$3,$4,$5)
       ON CONFLICT (game_id,player_id) DO NOTHING`,
      [u.game_id, u.player_id, u.view, u.version, u.status]);
  }
  const after = await viewsFor(gameId);
  for (const pid of [h1, h2]) assert.equal(after.get(pid)!.view, committed.get(pid)!.view, `row restored (${pid})`);

  // (b) Fill-if-absent must NOT overwrite an existing (newer) row: re-run with a
  // STALE version (0); every row already exists at the committed version.
  const stale = await buildPlayerViewUpserts(gameViewFromRow(row), row.state ?? null, 0);
  for (const u of stale) {
    await pgPool.query(
      `INSERT INTO player_views(game_id,player_id,view,version,status) VALUES($1,$2,$3,$4,$5)
       ON CONFLICT (game_id,player_id) DO NOTHING`,
      [u.game_id, u.player_id, u.view, u.version, u.status]);
  }
  const afterStale = await viewsFor(gameId);
  for (const pid of [h1, h2]) {
    assert.equal(afterStale.get(pid)!.version, committed.get(pid)!.version, `version untouched by stale fill (${pid})`);
    assert.equal(afterStale.get(pid)!.view, committed.get(pid)!.view, `view untouched by stale fill (${pid})`);
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

test('a dealt commit writes one fully-masked (no self) spectator row, byte-identical to the builder', async () => {
  const gameId = `sp${uuid().slice(0, 4)}`;
  const h1 = uuid(), h2 = uuid();
  await seedGame(gameId, [
    { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
    { id: h2, name: 'H2', is_ai: false, strategy_key: 'human' },
  ]);
  await executeWithGameLock(gameId, async (game) =>
    handleMetaAction({ user: { id: h1 } as any, user_name: 'H1', body: { type: 'start', game_id: gameId }, game, reqId: 'r' }),
    'r', false);

  const g = (await pgPool.query(`SELECT ${GAME_COLS} FROM games WHERE id=$1`, [gameId])).rows[0];
  const spec = await spectatorFor(gameId);
  assert.ok(spec, 'a spectator row was written');
  assert.equal(spec!.status, 'playing', 'status denormalized');
  assert.equal(spec!.version, String(g.version), 'version mirrors games.version');

  // Byte-identical to the shared builder's seat -1 envelope.
  const expected = await buildSpectatorView(gameViewFromRow(g), g.state ?? null, Number(g.version));
  assert.equal(spec!.view, expected, 'cached spectator view == buildSpectatorView output');

  // Decodes via the shared client codec as a spectator (no self, every hand
  // masked to a count only — the raw state never leaks to the public row).
  const decoded = decodePackedGame(hexToBytes(spec!.view));
  assert.ok(decoded, 'spectator row decodes');
  const view = decoded!.game as any;
  assert.equal(view.id, gameId);
  assert.equal(view.status, GAME_STATUS.PLAYING);
  assert.ok(!view.self, 'spectator has no self');
  assert.ok(view.players.every((p: any) => !p.hand), 'no player exposes real cards');
  assert.ok(view.players.some((p: any) => p.hand_length > 0), 'hands are present as counts');
});

test('create_game seeds a decodable, fully-masked spectator lobby row', async () => {
  const gameId = `sc${uuid().slice(0, 4)}`;
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
  const p_spectator = await buildSpectatorView(lobby, null, 0);
  const { error } = await supabaseClient.rpc('create_game', {
    p_game_id: gameId, p_name: lobby.name, p_player_id: h1,
    p_players: [{ player_id: h1, name: 'H1', status: PLAYER_STATUS.IDLE, is_ai: false }],
    p_views, p_spectator,
  });
  assert.ok(!error, `create_game ok: ${error?.message}`);

  const spec = await spectatorFor(gameId);
  assert.ok(spec, 'spectator lobby row seeded');
  assert.equal(spec!.status, 'waiting');
  const decoded = decodePackedGame(hexToBytes(spec!.view));
  assert.ok(decoded, 'lobby spectator row decodes');
  assert.equal(decoded!.game.status, GAME_STATUS.WAITING);
  assert.ok(!(decoded!.game as any).self, 'spectator lobby row has no self');
});

test('RLS: ANY authenticated user can read a spectator row, and clients cannot write', async () => {
  const gameId = `sr${uuid().slice(0, 4)}`;
  const h1 = uuid(), h2 = uuid(), outsider = uuid();
  await seedGame(gameId, [
    { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
    { id: h2, name: 'H2', is_ai: false, strategy_key: 'human' },
  ]);
  await executeWithGameLock(gameId, async (game) =>
    handleMetaAction({ user: { id: h1 } as any, user_name: 'H1', body: { type: 'start', game_id: gameId }, game, reqId: 'r' }),
    'r', false);

  const c = await pgPool.connect();
  try {
    // As an OUTSIDER (not a participant): still sees the shared spectator row —
    // that is the whole point (a non-participant spectating the game).
    await c.query('BEGIN');
    await c.query('SET LOCAL ROLE authenticated');
    await c.query(`SELECT set_config('request.jwt.claims', json_build_object('sub', $1::text, 'role', 'authenticated')::text, true)`, [outsider]);
    const seen = await c.query('SELECT game_id FROM spectator_views WHERE game_id=$1', [gameId]);
    assert.equal(seen.rows.length, 1, 'a non-participant can read the spectator row');

    // A client write is denied (no INSERT grant / policy).
    await assert.rejects(
      c.query(`INSERT INTO spectator_views(game_id,view,version,status) VALUES($1,'00',0,'playing')`, [`${gameId}x`]),
      /permission denied/, 'authenticated cannot insert');
    await c.query('ROLLBACK');
  } finally {
    c.release();
  }
});
