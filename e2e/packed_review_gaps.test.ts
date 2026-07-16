// Coverage for the packed-wire review findings (docs/PACKED_WIRE_CUTOVER.md):
//
//  1. The games-table column grants actually hide the unmasked state blob
//     (and the packed session log) from client roles — RLS can't hide a
//     column, so this is THE personalization boundary at the DB.
//  2. The binary action HTTP envelopes (request + response) round-trip and
//     reject garbage — the layer production clients actually POST.
//  3. guards.wasm's awire gate (validateActionWire) agrees with the server
//     kernel: every legal enumerated move validates 0, illegal moves return
//     a reject code, malformed wire returns -1.
//  4. buildPackedGameBytes (the get_game/get_my_games builder) + the games
//     list envelope: packed entries decode to exactly personalize_game's
//     view; WAITING rows refuse to serve a blob.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../supabase/functions/_shared/adapter/utils.ts';
import { handleMetaAction } from '../supabase/functions/_shared/adapter/meta_actions.ts';
import {
  Game, PersonalGame, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY, PrivatePlayer,
} from '../supabase/functions/_shared/core/types.ts';
import { personalize_game } from '../supabase/functions/_shared/common/common_utils.ts';
import { start_game } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import { kernelLegalMoves, kernelShouldAct, serializeGameState, __setKernelSeedSource } from '../sdk/ts/wasm/engine.ts';
import { encodeAction, decodeAction, encodeActionRequest, decodeActionRequest, encodeActionResponse, decodeActionResponse, ACTION_STATUS, AwireKindName } from '../sdk/ts/wire/awire.ts';
import { buildPackedGameBytes, gameViewFromRow } from '../supabase/functions/_shared/common/packed_game.ts';
import { decodePackedGame, encodeGamesList, decodePackedGamesList } from '../sdk/ts/wire/view.ts';
import { bytesToHex } from '../supabase/functions/_shared/common/replay/codec.ts';
import { validateActionWire, initClientGuards } from '../src/wasm/clientGuards.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

let seed = 0x5eed;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
const ri = (n: number) => Math.floor(rnd() * n);
__setKernelSeedSource(() => 777);

const mkPlayer = (i: number, isAi: boolean): PrivatePlayer => ({
  player_id: `player-${i}`, name: `P${i}`, status: PLAYER_STATUS.READY,
  is_ai: isAi, hand: [], awaiting_attack: false, hand_length: 0,
  strategy_key: isAi ? STRATEGY_KEY.RANDOM : STRATEGY_KEY.HUMAN,
});
const mkDealt = (n: number): Game => {
  const g: Game = {
    id: 'rg', name: 'rg', status: GAME_STATUS.WAITING,
    players: Array.from({ length: n }, (_, i) => mkPlayer(i, i % 2 === 1)),
    deck: [], deck_length: 0, discard_pile_length: 0, flipped: null,
    power_suit: 0, first_attacker: 0, defender: 0, table_battles: [],
    elimination_order: [], good_timestamp: null, good_players: [], logs: [],
  };
  start_game(g);
  g.status = GAME_STATUS.PLAYING;
  return g;
};

// ---- 2. binary HTTP envelopes (DB-free) -------------------------------------

test('awire HTTP envelopes: request/response round-trip, garbage rejected', () => {
  const wire = encodeAction({ kind: 'attack', cards: [{ suit: 2, value: 9 }] });
  const req = encodeActionRequest('abc123', wire);
  const parsed = decodeActionRequest(req);
  assert.ok(parsed, 'request decodes');
  assert.equal(parsed!.gameId, 'abc123');
  assert.deepEqual(Array.from(parsed!.wire), Array.from(wire), 'wire bytes verbatim');

  assert.equal(decodeActionRequest(new Uint8Array([])), null, 'empty rejected');
  assert.equal(decodeActionRequest(new Uint8Array([9, 3, 97, 98, 99, 0, 0])), null, 'bad format byte rejected');
  assert.equal(decodeActionRequest(req.subarray(0, 5)), null, 'truncated rejected');

  for (const [status, code, version] of [
    [ACTION_STATUS.APPLIED, 0, 0], [ACTION_STATUS.REJECTED, 13, 41],
    [ACTION_STATUS.MOOT, 0, 0x7fffffff], [ACTION_STATUS.APPLIED, 0, 0xffffffff],
  ] as const) {
    const resp = decodeActionResponse(encodeActionResponse(status, code, version));
    assert.ok(resp, 'response decodes');
    assert.equal(resp!.status, status);
    assert.equal(resp!.rejectCode, code);
    assert.equal(resp!.version, version, 'u32 version survives (incl. high bit)');
  }
  assert.equal(decodeActionResponse(new Uint8Array([1, 0, 0])), null, 'short response rejected');

  // decodeAction never throws on garbage.
  for (let i = 0; i < 5000; i++) {
    const buf = new Uint8Array(ri(12));
    for (let j = 0; j < buf.length; j++) buf[j] = ri(256);
    decodeAction(buf);
  }
});

// ---- 3. guards.wasm awire gate parity (DB-free) ------------------------------

test('validateActionWire: legal enumerated moves gate 0, illegal reject, malformed -1', async () => {
  await initClientGuards();
  let legal = 0, illegal = 0;
  for (let g = 0; g < 6; g++) {
    const game = mkDealt(2 + (g % 3));
    for (let mv = 0; mv < 30; mv++) {
      const actors = game.players.filter(p => kernelShouldAct(game, p.player_id));
      if (actors.length === 0) break;
      const actor = actors[ri(actors.length)];
      const seatIdx = game.players.findIndex(p => p.player_id === actor.player_id);
      const personal = personalize_game(game, actor.player_id) as PersonalGame;
      const menu = kernelLegalMoves(game, actor.player_id).filter(m => m.type !== 'wait');
      for (const m of menu.slice(0, 6)) {
        const wire = encodeAction({ kind: m.type as AwireKindName, cards: m.cards, attack_cards: m.attack_cards });
        assert.equal(validateActionWire(personal, wire), 0,
          `legal ${m.type} gates 0 (game ${g} move ${mv})`);
        legal++;
      }
      // A card the actor doesn't hold must reject.
      const foreign = encodeAction({ kind: 'attack', cards: [{ suit: 3, value: 13 }, { suit: 3, value: 13 }] });
      assert.ok(validateActionWire(personal, foreign) > 0, 'duplicate/foreign attack rejects');
      // Malformed wire is -1, never a crash.
      assert.equal(validateActionWire(personal, new Uint8Array([0, 9, 1])), -1, 'malformed wire is -1');
      illegal++;
      // Advance the game along a random legal move via the JS path.
      if (menu.length > 0) {
        const pick = menu[ri(menu.length)];
        const { handleAttack } = await import('../supabase/functions/_shared/common/actions/attack.ts');
        const { handleCover } = await import('../supabase/functions/_shared/common/actions/cover.ts');
        const { handlePass } = await import('../supabase/functions/_shared/common/actions/pass.ts');
        const { handlePickup } = await import('../supabase/functions/_shared/common/actions/pickup.ts');
        const { handleGood } = await import('../supabase/functions/_shared/common/actions/good.ts');
        try {
          switch (pick.type) {
            case 'attack': handleAttack(game, actor.player_id, pick.cards!); break;
            case 'cover': handleCover(game, actor.player_id, pick.cards!, pick.attack_cards!); break;
            case 'pass': handlePass(game, actor.player_id, pick.cards!); break;
            case 'pickup': handlePickup(game, actor.player_id); break;
            case 'good': handleGood(game, actor.player_id); break;
          }
        } catch { /* races don't exist here, but stay robust */ }
      }
      if (game.status !== GAME_STATUS.PLAYING) break;
    }
  }
  assert.ok(legal > 100, `gated enough legal moves (${legal})`);
  assert.ok(illegal > 20, `probed enough illegal wires (${illegal})`);
});

// ---- 4. packed game builder + list envelope (DB-free) ------------------------

test('buildPackedGameBytes matches personalize_game; WAITING rows refuse; list round-trips', async () => {
  const game = mkDealt(3);
  const row = {
    id: game.id, name: game.name, status: game.status, version: 7,
    state: bytesToHex(serializeGameState(game)),
    players: game.players.map(p => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai })),
    good_players: [], good_timestamp: null,
  };
  const me = game.players[0].player_id;
  const bytes = await buildPackedGameBytes(row, me);
  assert.ok(bytes, 'dealt row serves packed');
  const decoded = decodePackedGame(bytes!);
  assert.ok(decoded, 'packed game decodes');
  const expected = personalize_game(game, me) as PersonalGame;
  const got = decoded!.game as PersonalGame;
  assert.deepEqual(got.self.hand, expected.self.hand, 'own hand identical');
  assert.deepEqual(got.players.map(p => ({ ...p })), expected.players.map(p => ({ ...p })), 'public players identical');
  assert.deepEqual(got.table_battles, expected.table_battles);
  assert.equal(got.deck_length, expected.deck_length);
  assert.equal(decoded!.version, 7, 'envelope version');

  // Spectator view: no self, still decodes.
  const specBytes = await buildPackedGameBytes(row, 'not-a-player');
  const spec = decodePackedGame(specBytes!);
  assert.ok(spec && spec.seat === -1 && !(spec.game as PersonalGame).self, 'spectator packed view');

  // A WAITING row must never serve a blob (the stale-blob guard).
  assert.equal(await buildPackedGameBytes({ ...row, status: 'waiting' }, me), null, 'lobby refuses packed');
  assert.equal(await buildPackedGameBytes({ ...row, state: null }, me), null, 'blob-less refuses packed');

  // Games-list envelope: one packed + one JSON entry round-trip.
  const jsonEntry = new TextEncoder().encode(JSON.stringify(personalize_game(game, me)));
  const list = encodeGamesList([{ kind: 1, bytes: bytes! }, { kind: 0, bytes: jsonEntry }]);
  const games = decodePackedGamesList(list);
  assert.ok(games && games.length === 2, 'both entries decode');
  assert.equal(games![0].id, game.id);
  assert.equal(games![1].id, game.id);
  assert.equal(decodePackedGamesList(new Uint8Array([9, 1])), null, 'unknown list format is null');
});

// ---- 1. the DB grants (needs Postgres) ---------------------------------------

if (!process.env.VALIDATION_ONLY) {
  before(async () => { await applySchema(); });
  beforeEach(async () => { await resetDb(); });

  test('games.state and games.logs_packed are invisible to client roles', async () => {
    const gameId = `r${uuid().slice(0, 5)}`;
    const h1 = uuid(), h2 = uuid();
    await seedGame(gameId, [
      { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
      { id: h2, name: 'H2', is_ai: false, strategy_key: 'human' },
    ]);
    await executeWithGameLock(gameId, async (game) =>
      handleMetaAction({ user: { id: h1 } as any, user_name: 'H1', body: { type: 'start', game_id: gameId }, game, reqId: 'r' }), 'r', false);
    const dealt = await pgPool.query('SELECT state FROM games WHERE id=$1', [gameId]);
    assert.ok(dealt.rows[0].state, 'blob present (superuser sees it)');

    const c = await pgPool.connect();
    try {
      for (const role of ['authenticated', 'anon']) {
        await c.query('BEGIN');
        await c.query(`SET LOCAL ROLE ${role}`);
        await assert.rejects(c.query('SELECT state FROM games'), /permission denied/,
          `${role} cannot read the unmasked blob`);
        await c.query('ROLLBACK');

        await c.query('BEGIN');
        await c.query(`SET LOCAL ROLE ${role}`);
        await assert.rejects(c.query('SELECT logs_packed FROM games'), /permission denied/,
          `${role} cannot read the session log`);
        await c.query('ROLLBACK');

        await c.query('BEGIN');
        await c.query(`SET LOCAL ROLE ${role}`);
        await assert.rejects(c.query('SELECT * FROM games'), /permission denied/,
          `${role} cannot select * (it expands to hidden columns)`);
        await c.query('ROLLBACK');

        // The public columns stay readable — lobby/spectate listings work.
        await c.query('BEGIN');
        await c.query(`SET LOCAL ROLE ${role}`);
        const pub = await c.query('SELECT id, name, status, players, table_battles, version FROM games WHERE id=$1', [gameId]);
        assert.equal(pub.rows.length, 1, `${role} reads public columns`);
        await c.query('ROLLBACK');
      }
    } finally {
      c.release();
    }
  });

  // get_my_games serves any game with no state blob (a WAITING lobby, or a
  // finished/legacy game) straight from the games row via gameViewFromRow — never
  // loadCompleteGame, which would re-import supabase-js and do a per-game DB read
  // (the build-loop N+1 that cost ~150ms/game). Prove that reconstruction is
  // equivalent to the loadCompleteGame path it replaces, for BOTH a lobby and a
  // finished game, so the list a client sees is unchanged.
  test('gameViewFromRow(row) personalizes identically to loadCompleteGame (waiting + finished, no blob)', async () => {
    // The exact columns get_my_games selects.
    const cols = 'id,name,status,version,state,players,good_players,good_timestamp,' +
      'discard_pile_length,flipped,power_suit,first_attacker,defender,table_battles,elimination_order';

    // (a) WAITING lobby.
    const lobbyId = `l${uuid().slice(0, 5)}`;
    const h1 = uuid(), h2 = uuid();
    await seedGame(lobbyId, [
      { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
      { id: h2, name: 'B2', is_ai: true, strategy_key: STRATEGY_KEY.RANDOM },
    ]);

    // (b) Finished game with NO blob (the slow legacy shape): mark it GAME_OVER
    // with an elimination order and no state, exactly what falls through to the
    // row-view path in production.
    const overId = `o${uuid().slice(0, 5)}`;
    await seedGame(overId, [
      { id: h1, name: 'H1', is_ai: false, strategy_key: 'human' },
      { id: h2, name: 'B2', is_ai: true, strategy_key: STRATEGY_KEY.RANDOM },
    ]);
    await pgPool.query(
      `UPDATE games SET status='game_over', state=NULL, elimination_order=$2 WHERE id=$1`,
      [overId, JSON.stringify([h1])],
    );

    for (const [gameId, wantStatus] of [[lobbyId, GAME_STATUS.WAITING], [overId, GAME_STATUS.GAME_OVER]] as const) {
      const row = (await pgPool.query(`SELECT ${cols} FROM games WHERE id=$1`, [gameId])).rows[0];
      assert.equal(row.status, wantStatus, `${gameId} has expected status`);
      assert.equal(row.state, null, `${gameId} has no blob → row-view path`);

      const viaLoad = await loadCompleteGame(gameId);
      for (const viewer of [h1, h2, 'spectator-not-in-game']) {
        const fromRow = personalize_game(gameViewFromRow(row), viewer);
        const fromLoad = personalize_game(viaLoad, viewer);
        assert.deepEqual(fromRow, fromLoad, `${gameId}: personalized view matches for viewer ${viewer}`);
      }
    }
  });

  after(async () => { await pgPool.end(); });
}
