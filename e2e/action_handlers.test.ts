// The moves on the C Table: what table_act does with each kind of move on a
// deterministic mid-round board, and that it does nothing to a finished game or
// to a board it refuses.
//
// This file used to call the TS action-handler modules (actions/attack|cover|
// pass|pickup|good.ts: validateX, executeX, executeRoundTransition) on a
// TypeScript Game. Those were thin wrappers over the kernel and are gone with the
// TS game shape (docs/C_GAME_SHAPE_MIGRATION.md Phase 8); the move path the
// server runs is table_act on the loaded row (sdk/ts/table/server_table.ts), so
// the same cases are asked of it here, on boards the kernel built and sealed
// (e2e/helpers/table_fixture.ts). The per-viewer streams are held by
// e2e/push_as3.test.ts and S1 (e2e/helpers/hidden_info.ts).
//
// Pure kernel test - needs no Postgres.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import { boardFixture, MemTable, residentBoard, type MemCard, type BoardSpec } from './helpers/table_mem.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const C = (suit: number, value: number): MemCard => ({ suit, value });

// A playing 2-player board, diamonds trump, seat 0 attacks and seat 1 defends.
const base = (p0: MemCard[], p1: MemCard[], table: BoardSpec['table'] = [], status: number = L.GAME_STATUS_PLAYING): MemTable =>
  MemTable.of(boardFixture({ hands: [p0, p1], table, powerSuit: L.SUIT_DIAMONDS, attacker: 0, defender: 1, status }));

const uncovered = (t: MemTable) => t.board().battles.filter((b) => b.defense === null).length;

/** The move is refused (or moot) for `reject`, and the loaded board is exactly as it was loaded. */
function refusedWith(t: MemTable, seat: number, wire: Uint8Array, rc: number, reject: number | null, label: string): void {
  const before = t.board();
  const r = t.probe(seat, wire);
  assert.equal(r.rc, rc, `${label}: table_act result`);
  if (reject !== null) assert.equal(r.reject, reject, `${label}: the kernel's reason`);
  assert.deepEqual(residentBoard(), before, `${label}: nothing moved`);
}

// ---- attack -----------------------------------------------------------------

test('attack: a legal first attack applies; an empty or malformed one is refused and moves nothing', () => {
  const t = base([C(0, 5), C(0, 6), C(1, 7)], [C(2, 8), C(2, 9)]);
  assert.equal(t.probe(0, encodeAction({ kind: 'attack', cards: [C(0, 5)] })).rc, L.TABLE_APPLIED, 'a legal opener applies');
  refusedWith(t, 0, encodeAction({ kind: 'attack', cards: [] }), L.TABLE_REJECTED, L.ENGINE_REJECT_EMPTY, 'an empty attack');
  refusedWith(t, 0, Uint8Array.of(0, 2, 7), L.TABLE_E_WIRE, null, 'a truncated attack wire');
  assert.equal(t.board().battles.length, 0, 'the row still has an empty table');
});

test('attack: lands on a live game, is moot once the game is over', () => {
  const t = base([C(0, 5), C(0, 6), C(1, 7)], [C(2, 8), C(2, 9)]);
  const r = t.act(0, encodeAction({ kind: 'attack', cards: [C(0, 5)] }));
  assert.equal(r.rc, L.TABLE_APPLIED);
  const b = t.board();
  assert.equal(b.battles.length, 1, 'the attack card lands on the table');
  assert.deepEqual(b.battles[0].attack, C(0, 5), 'the card played is the card on the table');
  assert.equal(b.seats[0].hand.length, 2, 'and it left the hand');

  const over = base([C(0, 5), C(0, 6)], [C(2, 8), C(2, 9)], [], L.GAME_STATUS_GAME_OVER);
  refusedWith(over, 0, encodeAction({ kind: 'attack', cards: [C(0, 5)] }), L.TABLE_MOOT, null, 'an attack on a finished game');
});

// ---- cover ------------------------------------------------------------------

// One uncovered 7 of spades on the table; the defender holds the 9 of spades (covers) plus a spare.
const coverState = (status: number = L.GAME_STATUS_PLAYING) => base(
  [C(0, 5), C(0, 6), C(0, 8), C(0, 10), C(0, 11), C(0, 12)],
  [C(0, 9), C(2, 7)],
  [{ attack: C(0, 7), defense: null }],
  status,
);

test('cover: a legal cover applies; a card that cannot cover is refused; the wire pairs every cover with its attack', () => {
  const t = coverState();
  assert.equal(t.probe(1, encodeAction({ kind: 'cover', cards: [C(0, 9)], attack_cards: [C(0, 7)] })).rc, L.TABLE_APPLIED,
    'a legal cover applies');
  refusedWith(t, 1, encodeAction({ kind: 'cover', cards: [C(2, 7)], attack_cards: [C(0, 7)] }),
    L.TABLE_REJECTED, L.ENGINE_REJECT_CANNOT_COVER, 'an off-suit 7 over the 7 of spades');
  assert.throws(() => encodeAction({ kind: 'cover', cards: [C(0, 9), C(2, 7)], attack_cards: [C(0, 7)] }), /mismatched/,
    'covers and attacks of different sizes have no wire');
});

test('cover: covers on a live game, is moot once the game is over', () => {
  const t = coverState();
  assert.equal(t.act(1, encodeAction({ kind: 'cover', cards: [C(0, 9)], attack_cards: [C(0, 7)] })).rc, L.TABLE_APPLIED);
  assert.equal(uncovered(t), 0, 'the attack is now covered');

  const over = coverState(L.GAME_STATUS_GAME_OVER);
  refusedWith(over, 1, encodeAction({ kind: 'cover', cards: [C(0, 9)], attack_cards: [C(0, 7)] }), L.TABLE_MOOT, null, 'a cover on a finished game');
  assert.equal(uncovered(over), 1, 'the finished game is left uncovered');
});

// ---- pass -------------------------------------------------------------------

// One uncovered 7 of spades; the defender holds a 7 to pass; the next seat has room.
const passState = (status: number = L.GAME_STATUS_PLAYING) => base(
  [C(0, 5), C(0, 6), C(0, 8), C(0, 9), C(0, 10), C(0, 11)],
  [C(2, 7)],
  [{ attack: C(0, 7), defense: null }],
  status,
);

test('pass: a legal pass applies; an empty pass is refused', () => {
  const t = passState();
  assert.equal(t.probe(1, encodeAction({ kind: 'pass', cards: [C(2, 7)] })).rc, L.TABLE_APPLIED, 'a legal pass applies');
  refusedWith(t, 1, encodeAction({ kind: 'pass', cards: [] }), L.TABLE_REJECTED, L.ENGINE_REJECT_EMPTY, 'an empty pass');
});

test('pass: hands the defence on in a live game, is moot once the game is over', () => {
  const t = passState();
  assert.equal(t.act(1, encodeAction({ kind: 'pass', cards: [C(2, 7)] })).rc, L.TABLE_APPLIED);
  assert.equal(t.board().defender, 0, 'the pass hands the defence to the next seat');

  const over = passState(L.GAME_STATUS_GAME_OVER);
  refusedWith(over, 1, encodeAction({ kind: 'pass', cards: [C(2, 7)] }), L.TABLE_MOOT, null, 'a pass on a finished game');
  assert.equal(over.board().defender, 1, 'the finished game keeps its defender');
});

// ---- pickup -----------------------------------------------------------------

test('pickup: the defender takes a non-empty table; an attacker may not', () => {
  const t = base(
    [C(0, 5), C(0, 6), C(0, 8), C(0, 10), C(0, 11), C(0, 12)],
    [C(2, 7)],
    [{ attack: C(0, 7), defense: null }],
  );
  refusedWith(t, 0, encodeAction({ kind: 'pickup' }), L.TABLE_REJECTED, L.ENGINE_REJECT_NOT_DEFENDER, 'a pickup by the attacker');
  const r = t.act(1, encodeAction({ kind: 'pickup' }));
  assert.equal(r.rc, L.TABLE_APPLIED, 'the defender takes the table');
  const b = t.board();
  assert.equal(b.battles.length, 0, 'the table is empty');
  assert.ok(b.seats[1].hand.some((c) => c.suit === 0 && c.value === 7), 'and the attack is in the defender\'s hand');
});

// ---- the round's end --------------------------------------------------------

test('good: a covered table is discarded when the attacker says good, and is moot once the game is over', () => {
  const covered = (status: number = L.GAME_STATUS_PLAYING) => base(
    [C(0, 5), C(0, 6), C(0, 8), C(0, 10), C(0, 11), C(0, 12)],
    [C(2, 8), C(2, 9), C(2, 10), C(2, 11), C(2, 12), C(1, 5)],
    [{ attack: C(0, 7), defense: C(0, 9) }],
    status,
  );
  const t = covered();
  assert.equal(t.act(0, encodeAction({ kind: 'good' })).rc, L.TABLE_APPLIED);
  const b = t.board();
  assert.equal(b.battles.length, 0, 'the covered table is discarded');
  assert.equal(b.discard, 2, 'both cards go to the discard pile');

  const over = covered(L.GAME_STATUS_GAME_OVER);
  refusedWith(over, 0, encodeAction({ kind: 'good' }), L.TABLE_MOOT, null, 'a good on a finished game');
  assert.equal(over.board().battles.length, 1, 'the finished game keeps its table');
});
