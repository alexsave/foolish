// Coverage for the packed-wire review findings (docs/PACKED_WIRE_CUTOVER.md),
// on the C Table (docs/C_GAME_SHAPE_MIGRATION.md Phase 4b):
//
//  1. The games-table column grants actually hide the unmasked state blob, the
//     roster, the deal seed and the packed session log from client roles - RLS
//     can't hide a column, so this is THE personalization boundary at the DB.
//  2. The binary action HTTP envelopes (request + response) round-trip and
//     reject garbage - the layer production clients actually POST - and the
//     kernel's request decoder refuses the same garbage.
//  3. The client's awire gate (the kernel's client_validate on the board the
//     client holds) agrees with the server kernel: every legal move the kernel
//     enumerates validates 0 against the view the kernel serves that seat,
//     illegal moves return a reject code, malformed wire is CLIENT_E_MOVE.
//  4. The envelope the kernel serves a seat (and a spectator) decodes to exactly
//     that seat's view of the board the kernel holds, for a dealt game, a lobby
//     and a finished game.
//
// DELETED with their TS twin (plan Phase 4b): the `gameViewFromRow(row)
// personalizes identically to loadCompleteGame` cases (a lobby, and a finished
// game with no blob). Both TS row readers are gone: every envelope a client is
// served, list or single game, is the kernel's table_envelope / commit product.
// Now covered by case 4 below (a lobby and a finished board, every seat and a
// spectator), and by e2e/table_expand_migration.test.ts for the rows the hosted
// database holds.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { isCard } from '../src/state/view.ts';
import { fixture, fixtureTable, GAME_OVER, IDLE, READY, type TableFixture } from './helpers/table_fixture.ts';
import { legalMoves, residentBoard, type BoardState, type PlayCard } from './helpers/table_play.ts';
import { runMeta, seedLobby } from './helpers/table_server.ts';
import { suiteRng } from './helpers/rng.ts';
import { encodeAction, decodeAction, encodeActionRequest, decodeActionRequest, encodeActionResponse, decodeActionResponse, ACTION_STATUS } from '../sdk/ts/wire/awire.ts';
import { readEnvelopeView } from './helpers/client_read.ts';
import { clientTable } from '../sdk/ts/table/client_table.ts';
import * as V from '../sdk/ts/gen/view_layout.bots.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const rng = suiteRng('packed_review_gaps');
const ri = (n: number) => rng.int(n);

const sameCards = (a: readonly { suit: number; value: number }[], b: PlayCard[]) =>
  assert.deepEqual(a.map((c) => [c.suit, c.value]), b.map((c) => [c.suit, c.value]));

/**
 * An in-memory game on the fixtures' C Table: `n` seats (bots on odd seats),
 * every seat but the first READY, and the first seat's ready deals it from a
 * seed drawn from the suite stream. Returns the dealt table's bytes.
 */
function dealt(gameId: string, n: number): TableFixture {
  const t = fixtureTable();
  let b = fixture().title(gameId).seats(Array.from({ length: n }, (_, i) =>
    ({ id: `player-${i}`, name: `P${i}`, brain: i % 2 === 1 ? 'random' : '' })));
  for (let i = 1; i < n; i++) b = b.seatStatus(i, READY);
  const lobby = b.build();
  assert.equal(t.load(lobby.state, lobby.roster), L.TABLE_OK, 'lobby loads');
  assert.equal(t.ready('player-0', Uint8Array.from({ length: 32 }, () => ri(256))), L.TABLE_OK, 'the last ready deals');
  return committed(gameId);
}

/** The loaded table's state and roster, after its last operation. */
function committed(gameId: string): TableFixture {
  const p = fixtureTable().commit(gameId, 1, 0);
  if (typeof p === 'number') throw new Error(`no commit products (${p})`);
  return { state: p.state, roster: p.roster };
}

/** Loads `fx` and returns the board it holds. */
function boardOf(gameId: string, fx: TableFixture): BoardState {
  assert.equal(fixtureTable().load(fx.state, fx.roster), L.TABLE_OK, 'table loads');
  return residentBoard(gameId, fx.state, fx.roster);
}

/** The envelope the kernel serves `seat` (-1: a spectator) of `fx`, as the board the web holds (what the guards gate reads). */
function servedView(gameId: string, fx: TableFixture, seat: number, version: number) {
  assert.equal(fixtureTable().load(fx.state, fx.roster), L.TABLE_OK, 'table loads');
  const env = fixtureTable().envelope(gameId, seat, version);
  if (typeof env === 'number') throw new Error(`envelope refused (${env})`);
  const view = readEnvelopeView(env);
  assert.ok(view, 'the envelope reads');
  return view!;
}

// ---- 2. binary HTTP envelopes (DB-free) -------------------------------------

test('awire HTTP envelopes: request/response round-trip, garbage rejected (by the TS codec and the kernel)', () => {
  const wire = encodeAction({ kind: 'attack', cards: [{ suit: 2, value: 9 }] });
  const req = encodeActionRequest('abc123', wire);
  const parsed = decodeActionRequest(req);
  assert.ok(parsed, 'request decodes');
  assert.equal(parsed!.gameId, 'abc123');
  assert.deepEqual(Array.from(parsed!.wire), Array.from(wire), 'wire bytes verbatim');
  const kernel = fixtureTable().requestDecode(req);
  assert.ok(typeof kernel !== 'number', 'the kernel decodes the same request');
  assert.equal(kernel.gameId, 'abc123');
  assert.deepEqual(Array.from(kernel.wire), Array.from(wire), 'the kernel reads the same wire');

  for (const [label, garbage] of [
    ['empty', new Uint8Array([])],
    ['bad format byte', new Uint8Array([9, 3, 97, 98, 99, 0, 0])],
    ['truncated', req.subarray(0, 5)],
  ] as const) {
    assert.equal(decodeActionRequest(garbage), null, `${label} rejected`);
    assert.equal(typeof fixtureTable().requestDecode(garbage), 'number', `${label} refused by the kernel`);
  }

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

// ---- 3. client awire gate parity (DB-free) ----------------------------------

const validateActionWire = (view: Parameters<ReturnType<typeof clientTable>['validate']>[0], wire: Uint8Array): number =>
    clientTable().validate(view, wire);

test('client validate: legal enumerated moves gate 0, illegal reject, malformed is refused', async () => {
  let legal = 0, illegal = 0;
  for (let g = 0; g < 6; g++) {
    const gameId = `rg${g}`;
    let fx = dealt(gameId, 2 + (g % 3));
    for (let mv = 0; mv < 30; mv++) {
      const moves = legalMoves(boardOf(gameId, fx));
      const actors = [...new Set(moves.map((m) => m.seat))];
      if (actors.length === 0) break;
      const seat = actors[ri(actors.length)];
      const personal = servedView(gameId, fx, seat, 1);
      const menu = moves.filter((m) => m.seat === seat);
      for (const m of menu.slice(0, 6)) {
        assert.equal(validateActionWire(personal, m.wire), 0,
          `legal ${m.kind} gates 0 (game ${g} move ${mv}, seed=${rng.seed})`);
        legal++;
      }
      // A card the actor doesn't hold must reject.
      const foreign = encodeAction({ kind: 'attack', cards: [{ suit: 3, value: 13 }, { suit: 3, value: 13 }] });
      assert.ok(validateActionWire(personal, foreign) > 0, 'duplicate/foreign attack rejects');
      // Malformed wire is -1, never a crash.
      assert.equal(validateActionWire(personal, new Uint8Array([0, 9, 1])), V.CLIENT_E_MOVE, 'malformed wire is refused as no move');
      illegal++;
      // Advance the game along a random legal move on the kernel.
      const pick = menu[ri(menu.length)];
      const t = fixtureTable();
      assert.equal(t.load(fx.state, fx.roster), L.TABLE_OK);
      assert.equal(t.act(pick.playerId, pick.wire, null, 0), L.TABLE_APPLIED, `an enumerated ${pick.kind} applies`);
      fx = committed(gameId);
      if (boardOf(gameId, fx).status !== L.GAME_STATUS_PLAYING) break;
    }
  }
  assert.ok(legal > 100, `gated enough legal moves (${legal}, seed=${rng.seed})`);
  assert.ok(illegal > 20, `probed enough illegal wires (${illegal}, seed=${rng.seed})`);
});

// ---- 4. the served envelope is the seat's view of the kernel's board (DB-free) ----

test('the kernel envelope of every seat and a spectator decodes to its view of the board (dealt, lobby, finished)', () => {
  const ids = ['player-0', 'player-1', 'player-2'];

  // (a) A dealt game, a few moves in.
  const gameId = 'rg-env';
  let fx = dealt(gameId, 3);
  for (let i = 0; i < 4; i++) {
    const moves = legalMoves(boardOf(gameId, fx));
    if (moves.length === 0) break;
    const m = moves[ri(moves.length)];
    fixtureTable().load(fx.state, fx.roster);
    fixtureTable().act(m.playerId, m.wire, null, 0);
    fx = committed(gameId);
  }
  const board = boardOf(gameId, fx);
  assert.equal(board.status, L.GAME_STATUS_PLAYING, 'still in play');
  board.seats.forEach((s, seat) => {
    const got = servedView(gameId, fx, seat, 7);
    assert.equal(got.version, 7, 'envelope version');
    assert.equal(got.mySeat, seat, 'envelope seat');
    sameCards(got.myHand, s.hand);
    assert.deepEqual(got.seats.map((p) => [p.id, p.name, p.handCount, p.isAi]),
      board.seats.map((x) => [x.id, x.name, x.hand.length, x.brain !== '']), 'public players');
    assert.equal(got.battles.length, board.battles.length, 'battle count');
    got.battles.forEach((b, i) => {
      sameCards([b.attack], [board.battles[i].attack]);
      sameCards(isCard(b.defense) ? [b.defense] : [], board.battles[i].defense ? [board.battles[i].defense!] : []);
    });
    assert.equal(got.deckCount, board.deckCount, 'deck length');
    assert.equal(got.defender, board.defender, 'defender');
    assert.equal(got.firstAttacker, board.firstAttacker, 'first attacker');
    assert.equal(got.status, L.GAME_STATUS_PLAYING);
  });
  const spec = servedView(gameId, fx, -1, 7);
  assert.equal(spec.mySeat, -1, 'spectator envelope');
  assert.deepEqual(spec.myHand, [], 'a spectator is served no hand');
  assert.deepEqual(spec.seats.map((p) => p.handCount), board.seats.map((s) => s.hand.length), 'but every count');

  // (b) A lobby, and (c) a finished game: every viewer reads the table the kernel
  // holds, with no card anywhere.
  const lobby = fixture().title('lobby').seats([{ id: ids[0], name: 'H1' }, { id: ids[1], name: 'B2', brain: 'random' }]).build();
  const over = fixture().title('over').seats([{ id: ids[0], name: 'H1' }, { id: ids[1], name: 'B2', brain: 'random' }])
    .status(GAME_OVER).eliminated(0).discard(36).seatStatus(0, IDLE).seatStatus(1, READY).build();
  for (const [label, row, status] of [['lobby', lobby, L.GAME_STATUS_WAITING], ['finished', over, L.GAME_STATUS_GAME_OVER]] as const) {
    const b = boardOf(label, row);
    // The builder's statuses: a human IDLE, a bot READY, in both tables.
    assert.deepEqual(b.seats.map((s) => s.status), [IDLE, READY], `${label}: the fixture's seat statuses`);
    for (const seat of [0, 1, -1]) {
      const d = servedView(label, row, seat, 3);
      assert.equal(d.mySeat, seat, `${label}: envelope seat ${seat}`);
      assert.equal(d.status, status, `${label}: status`);
      assert.equal(d.title, b.title, `${label}: title`);
      assert.deepEqual(d.seats.map((p) => [p.id, p.name, p.status, p.handCount, p.isAi]),
        b.seats.map((s) => [s.id, s.name, s.status, 0, s.brain !== '']),
        `${label}: the roster and seat statuses (viewer ${seat})`);
      assert.equal(d.battles.length, 0, `${label}: no table`);
      assert.deepEqual(d.myHand, [], `${label}: no hand`);
    }
  }
});

// ---- 1. the DB grants (needs Postgres) ---------------------------------------

if (!process.env.VALIDATION_ONLY) {
  before(async () => { await applySchema(); });
  beforeEach(async () => { await resetDb(); });

  test('games.state, roster, game_seed and logs_packed are invisible to client roles', async () => {
    const gameId = `r${uuid().slice(0, 5)}`;
    const h1 = uuid(), h2 = uuid();
    await seedLobby(gameId, [{ id: h1, name: 'H1', ready: false }, { id: h2, name: 'H2' }]);
    await runMeta(gameId, h1, { type: 'start' });
    const dealtRow = await pgPool.query('SELECT state, roster, game_seed, logs_packed FROM games WHERE id=$1', [gameId]);
    assert.ok(dealtRow.rows[0].state && dealtRow.rows[0].roster && dealtRow.rows[0].game_seed && dealtRow.rows[0].logs_packed,
      'blob, roster, seed and log present (superuser sees them)');

    const c = await pgPool.connect();
    try {
      for (const role of ['authenticated', 'anon']) {
        for (const [column, what] of [
          ['state', 'the unmasked blob'], ['roster', 'the roster blob'], ['game_seed', 'the deal seed'],
          ['logs_packed', 'the session log'], ['*', 'select * (it expands to hidden columns)'],
        ] as const) {
          await c.query('BEGIN');
          await c.query(`SET LOCAL ROLE ${role}`);
          await assert.rejects(c.query(`SELECT ${column} FROM games`), /permission denied/,
            `${role} cannot read ${what}`);
          await c.query('ROLLBACK');
        }

        // No column is readable at all since the contract migration: lobby and
        // spectate listings read player_views / spectator_views.
        for (const column of ['id', 'status', 'version']) {
          await c.query('BEGIN');
          await c.query(`SET LOCAL ROLE ${role}`);
          await assert.rejects(c.query(`SELECT ${column} FROM games WHERE id=$1`, [gameId]), /permission denied/,
            `${role} cannot read games.${column}`);
          await c.query('ROLLBACK');
        }
      }
    } finally {
      c.release();
    }
  });

  after(async () => { await pgPool.end(); });
}
