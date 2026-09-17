// The kernel refuses a Game it could never have produced.
//
// Every server path hands the C kernel a game as bytes: marshalGame (the JS
// Game -> transient IO import) on the action and bot paths, and the durable
// games.state blob on the packed paths. The decoder clamps COUNTS so it cannot
// corrupt memory, but it used to adopt any VALUE as it came - a defender seat
// past the table, an eliminated player id that is not seated (marshalGame wrote
// findIndex's -1 straight into the wire), a status string nobody knows, a card
// byte that is no card - and then play on it.
//
// These drive the real engine.ts entry points and assert three things: the
// import is refused, the refusal names the reason (the kernel's
// GAME_INVALID_* code, c/src/game.h game_validate), and the kernel did not
// adopt the refused state - the game resident before the refusal is still the
// one resident after it. The value checks live in C; nothing here re-derives
// them. Pure kernel test - no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { start_game } from '../server/api/common/game_lifecycle.ts';
import {
  kernelAttack, kernelHumanMask, runPackedAction, runPackedGameAction,
  serializeGameState, deserializeGameState, serializeViewBlob,
  exportPackedDriveProducts, kernelResetToLobby, RosterTemplate,
} from '../sdk/ts/wasm/engine.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import { Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../server/api/core/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const mkPlayer = (i: number): PrivatePlayer => ({
  player_id: `player-${i}`, name: `Player ${i}`, status: PLAYER_STATUS.READY, is_ai: false,
  hand: [], awaiting_attack: false, hand_length: 0, strategy_key: STRATEGY_KEY.RANDOM,
});

// A dealt 4-seat game: trump up, hands of six, first attacker to move.
function dealt(): Game {
  const g: Game = {
    players: Array.from({ length: 4 }, (_, i) => mkPlayer(i)),
    deck: [], logs: [], id: 'validate', name: 'validate', status: GAME_STATUS.WAITING,
    deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
    first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
    good_timestamp: null, good_players: [], version: 1,
  };
  start_game(g);
  assert.equal(g.status, GAME_STATUS.PLAYING, 'fixture is dealt');
  return g;
}

const rosterOf = (g: Game): RosterTemplate => ({
  id: g.id, name: g.name, version: g.version, deck_length: g.deck.length,
  players: g.players.map(p => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai, strategy_key: p.strategy_key })),
  good_players: g.good_players, good_timestamp: g.good_timestamp,
});

// The attack the first attacker can always make on a fresh deal.
const openingAttack = (g: Game) => {
  const fa = g.players[g.first_attacker];
  return { id: fa.player_id, seat: g.first_attacker, card: fa.hand[0] };
};

const INVALID = (what: RegExp) => (e: unknown) => {
  assert.ok(e instanceof Error, 'refusal is an Error');
  assert.match(e.message, /^Invalid game state: /, `names the refusal (got: ${e.message})`);
  assert.match(e.message, what, `names the reason (got: ${e.message})`);
  return true;
};

// Byte offsets in the durable blob: [version][det flag][state_put...].
const BLOB = { status: 2, firstAttacker: 5, defender: 6, deck0: 18 };

// ---- the JS Game path (marshalGame -> wasm_import_state) --------------------

test('a defender seat past the table is refused on the action path', () => {
  const g = dealt();
  const { id, card } = openingAttack(g);
  g.defender = g.players.length;
  assert.throws(() => kernelAttack(g, id, [card]), INVALID(/seat/i));
});

test('an eliminated player id that is not seated is refused, not written as seat -1', () => {
  const g = dealt();
  const { id, card } = openingAttack(g);
  g.elimination_order = ['nobody-sits-here'];
  assert.throws(() => kernelAttack(g, id, [card]), INVALID(/elimination/i));
});

test('a good player id that is not seated is refused, not silently dropped', () => {
  const g = dealt();
  const { id, card } = openingAttack(g);
  g.good_players = ['nobody-sits-here'];
  assert.throws(() => kernelAttack(g, id, [card]), INVALID(/good/i));
});

test('a game status outside the enum is refused, not read as waiting', () => {
  const g = dealt();
  const { id, card } = openingAttack(g);
  (g as { status: string }).status = 'paused';
  assert.throws(() => kernelAttack(g, id, [card]), INVALID(/status/i));
});

test('a player status outside the enum is refused, not read as idle', () => {
  const g = dealt();
  const { id, card } = openingAttack(g);
  (g.players[2] as { status: string }).status = 'asleep';
  assert.throws(() => kernelAttack(g, id, [card]), INVALID(/player status/i));
});

test('a card that is not a card is refused, not clamped onto the ace of diamonds', () => {
  const g = dealt();
  const { id, card } = openingAttack(g);
  const other = (g.first_attacker + 1) % g.players.length;
  g.players[other].hand[0] = { suit: 99, value: 99 };
  assert.throws(() => kernelAttack(g, id, [card]), INVALID(/card/i));
});

test('one card in two hands is refused', () => {
  const g = dealt();
  const { id, seat, card } = openingAttack(g);
  const other = (seat + 1) % g.players.length;
  g.players[other].hand[0] = { ...g.players[seat].hand[1] };
  assert.throws(() => kernelAttack(g, id, [card]), INVALID(/duplicate/i));
});

test('a first attacker seat past the table is refused on the packed game path', () => {
  const g = dealt();
  const { seat, card } = openingAttack(g);
  g.first_attacker = 9;
  assert.throws(() => runPackedGameAction(g, seat, encodeAction({ kind: 'attack', cards: [card] }), 0, [0, 1, 2, 3]),
    INVALID(/seat/i));
});

test('a refused JS Game leaves the previously imported game resident', () => {
  // Deal both first: a deal runs the kernel, so it would replace the resident.
  const good = dealt();
  const bad = dealt();
  const { id, card } = openingAttack(bad);
  bad.defender = 7;
  const expected = serializeGameState(good);
  kernelHumanMask(good);                       // marshal `good`: now resident

  try { kernelAttack(bad, id, [card]); } catch { /* the refusal is the other tests' business */ }

  // exportPackedDriveProducts reads whatever the kernel holds, without a marshal.
  const resident = exportPackedDriveProducts(-1, 0, [], 0).stateBlob;
  assert.deepEqual([...resident], [...expected], 'the refused game was not adopted');
});

// ---- the durable blob path (wasm_state_deserialize) -------------------------

test('a durable blob with a status byte outside the enum is refused', () => {
  const g = dealt();
  const blob = serializeGameState(g);
  blob[BLOB.status] = 9;
  assert.throws(() => deserializeGameState(blob, rosterOf(g)), INVALID(/status/i));
});

test('a durable blob with a defender past the table is refused on the packed action path', () => {
  const g = dealt();
  const { seat, card } = openingAttack(g);
  const blob = serializeGameState(g);
  blob[BLOB.defender] = 200;
  assert.throws(() => runPackedAction(blob, seat, encodeAction({ kind: 'attack', cards: [card] }), 0, [0, 1, 2, 3]),
    INVALID(/seat/i));
});

test('a durable blob with a deck byte that is no card is refused on the view path', () => {
  const g = dealt();
  assert.ok(g.deck.length > 0, 'fixture has a deck');
  const blob = serializeGameState(g);
  blob[BLOB.deck0] = 0x80;
  assert.throws(() => serializeViewBlob(blob, 0), INVALID(/card/i));
});

test('a refused durable blob leaves the previously loaded game resident', () => {
  // Build both blobs first: a deal runs the kernel, so it would replace the resident.
  const good = serializeGameState(dealt());
  const bad = serializeGameState(dealt());
  bad[BLOB.firstAttacker] = 0xff;
  serializeViewBlob(good, -1);                 // load `good`: now resident

  try { serializeViewBlob(bad, -1); } catch { /* refused */ }

  const resident = exportPackedDriveProducts(-1, 0, [], 0).stateBlob;
  assert.deepEqual([...resident], [...good], 'the refused blob was not adopted');
});

// ---- the refusal does not reach legitimate games ----------------------------

test('a dealt game, its blob and its lobby reset all still import', () => {
  const g = dealt();
  const blob = serializeGameState(g);
  const back = deserializeGameState(blob, rosterOf(g));
  assert.deepEqual([...serializeGameState(back)], [...blob], 'blob round-trips');
  assert.doesNotThrow(() => serializeViewBlob(blob, 1));
  const { id, card } = openingAttack(g);
  assert.doesNotThrow(() => kernelAttack(g, id, [card]));
  kernelResetToLobby(g);
  assert.equal(g.status, GAME_STATUS.WAITING, 'reset reached the lobby');
  assert.doesNotThrow(() => serializeGameState(g), 'the lobby imports');
});
