// The action-handler modules (actions/attack|cover|pass|pickup|good.ts) are
// thin wrappers over the C rules kernel. The e2e suite drives them through the
// combined handleX() path via dispatch, so the standalone validateX()/
// executeX()/executeRoundTransition() exports the client and server also import
// — plus the payload-shape guards (not-playing, empty, size-mismatch) and the
// game-over short-circuits — went unexercised.
//
// This file constructs deterministic mid-round states and calls those exports
// directly. Pure kernel test — needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  Game, PrivatePlayer, Card, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
} from '../supabase/functions/_shared/core/types.ts';
import { validateAttack, executeAttack } from '../supabase/functions/_shared/common/actions/attack.ts';
import { validateCover, executeCover } from '../supabase/functions/_shared/common/actions/cover.ts';
import { validatePass, executePass } from '../supabase/functions/_shared/common/actions/pass.ts';
import { validatePickup } from '../supabase/functions/_shared/common/actions/pickup.ts';
import { executeRoundTransition } from '../supabase/functions/_shared/common/actions/good.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const C = (suit: number, value: number): Card => ({ suit, value });

const mkPlayer = (i: number, hand: Card[]): PrivatePlayer => ({
  player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.IN, is_ai: true,
  hand, awaiting_attack: false, hand_length: hand.length,
  strategy_key: STRATEGY_KEY.RANDOM,
});

// Playing 2-player game, diamonds trump, seat 0 attacks / seat 1 defends.
const baseGame = (p0: Card[], p1: Card[], table: Game['table_battles'] = []): Game => ({
  players: [mkPlayer(0, p0), mkPlayer(1, p1)],
  deck: [], logs: [], id: 'ah', name: 'ah', status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 3,
  first_attacker: 0, defender: 1, table_battles: table,
  elimination_order: [], good_timestamp: null, good_players: [],
});

const clone = (g: Game): Game => structuredClone(g);
const uncovered = (g: Game) => g.table_battles.filter(b => b.defense === null).length;

// ---- attack -----------------------------------------------------------------

test('validateAttack: accepts a legal first attack, rejects empty/malformed', () => {
  const g = baseGame([C(0, 5), C(0, 6), C(1, 7)], [C(2, 8), C(2, 9)]);
  assert.doesNotThrow(() => validateAttack(g, 'p0', [C(0, 5)]), 'a legal opener validates');
  assert.throws(() => validateAttack(g, 'p0', []), /no cards/i, 'empty rejected');
  assert.throws(
    () => validateAttack(g, 'p0', 'junk' as unknown as Card[]),
    /must be an array/i, 'malformed payload rejected',
  );
  // validation never mutates.
  assert.equal(g.table_battles.length, 0, 'validate leaves the table empty');
});

test('executeAttack: applies on a live game, is a no-op once the game is over', () => {
  const g = baseGame([C(0, 5), C(0, 6), C(1, 7)], [C(2, 8), C(2, 9)]);
  const events = executeAttack(g, 'p0', [C(0, 5)]);
  assert.ok(events.length > 0, 'a live attack emits animation events');
  assert.equal(g.table_battles.length, 1, 'the attack card lands on the table');
  assert.equal(g.table_battles[0].attack.value, 5, 'correct card on the table');

  const over = baseGame([C(0, 5), C(0, 6)], [C(2, 8), C(2, 9)]);
  over.status = GAME_STATUS.GAME_OVER;
  const before = clone(over);
  assert.deepEqual(executeAttack(over, 'p0', [C(0, 5)]), [], 'no events on a finished game');
  assert.deepEqual(over.table_battles, before.table_battles, 'finished game is untouched');
});

// ---- cover ------------------------------------------------------------------

// One uncovered 7♠ on the table; defender holds 9♠ (covers) plus a spare.
const coverState = () => baseGame(
  [C(0, 5), C(0, 6), C(0, 8), C(0, 10), C(0, 11), C(0, 12)],
  [C(0, 9), C(2, 7)],
  [{ attack: C(0, 7), defense: null }],
);

test('validateCover: accepts a legal cover, enforces playing-state and paired shapes', () => {
  const g = coverState();
  assert.doesNotThrow(() => validateCover(g, 'p1', [C(0, 9)], [C(0, 7)]), 'a legal cover validates');

  assert.throws(
    () => validateCover(g, 'p1', [C(0, 9), C(2, 7)], [C(0, 7)]),
    /different sizes/i, 'cover/attack length mismatch rejected',
  );

  const waiting = coverState();
  waiting.status = GAME_STATUS.WAITING;
  assert.throws(
    () => validateCover(waiting, 'p1', [C(0, 9)], [C(0, 7)]),
    /not in playing state/i, 'cover on a non-playing game rejected',
  );
});

test('executeCover: covers on a live game, is a no-op once the game is over', () => {
  const g = coverState();
  const events = executeCover(g, 'p1', [C(0, 9)], [C(0, 7)]);
  assert.ok(events.length > 0, 'a live cover emits events');
  assert.equal(uncovered(g), 0, 'the attack is now covered');

  const over = coverState();
  over.status = GAME_STATUS.GAME_OVER;
  assert.deepEqual(executeCover(over, 'p1', [C(0, 9)], [C(0, 7)]), [], 'no events on a finished game');
  assert.equal(uncovered(over), 1, 'finished game left uncovered');
});

// ---- pass -------------------------------------------------------------------

// One uncovered 7♠; defender holds a 7 to transfer; next seat has capacity.
const passState = () => baseGame(
  [C(0, 5), C(0, 6), C(0, 8), C(0, 9), C(0, 10), C(0, 11)],
  [C(2, 7)],
  [{ attack: C(0, 7), defense: null }],
);

test('validatePass: accepts a legal transfer, enforces playing-state and non-empty', () => {
  const g = passState();
  assert.doesNotThrow(() => validatePass(g, 'p1', [C(2, 7)]), 'a legal pass validates');

  const waiting = passState();
  waiting.status = GAME_STATUS.WAITING;
  assert.throws(() => validatePass(waiting, 'p1', [C(2, 7)]), /not in playing state/i, 'pass on a non-playing game rejected');
  assert.throws(() => validatePass(g, 'p1', []), /no cards/i, 'empty pass rejected');
});

test('executePass: transfers on a live game, is a no-op once the game is over', () => {
  const g = passState();
  const events = executePass(g, 'p1', [C(2, 7)]);
  assert.ok(events.length > 0, 'a live pass emits events');
  assert.equal(g.defender, 0, 'the pass hands the defence to the next seat');

  const over = passState();
  over.status = GAME_STATUS.GAME_OVER;
  assert.deepEqual(executePass(over, 'p1', [C(2, 7)]), [], 'no events on a finished game');
  assert.equal(over.defender, 1, 'finished game keeps its defender');
});

// ---- pickup -----------------------------------------------------------------

test('validatePickup: accepts a defender scooping a non-empty table', () => {
  const g = baseGame(
    [C(0, 5), C(0, 6), C(0, 8), C(0, 10), C(0, 11), C(0, 12)],
    [C(2, 7)],
    [{ attack: C(0, 7), defense: null }],
  );
  assert.doesNotThrow(() => validatePickup(g, 'p1'), 'a legal pickup validates');
  assert.throws(() => validatePickup(g, 'p0'), /./, 'a non-defender pickup is rejected');
});

// ---- round transition (good.ts standalone export) ---------------------------

test('executeRoundTransition: discards a covered table, is a no-op once the game is over', () => {
  const g = baseGame(
    [C(0, 5), C(0, 6), C(0, 8), C(0, 10), C(0, 11), C(0, 12)],
    [C(2, 8), C(2, 9), C(2, 10), C(2, 11), C(2, 12), C(1, 5)],
    [{ attack: C(0, 7), defense: C(0, 9) }],   // fully covered
  );
  const events = executeRoundTransition(g, 'test');
  assert.ok(events.length > 0, 'the transition emits events');
  assert.equal(g.table_battles.length, 0, 'the covered table is discarded');
  assert.equal(g.discard_pile_length, 2, 'both cards go to the discard pile');

  const over = baseGame([C(0, 5)], [C(2, 8)], [{ attack: C(0, 7), defense: C(0, 9) }]);
  over.status = GAME_STATUS.GAME_OVER;
  assert.deepEqual(executeRoundTransition(over, 'test'), [], 'no events on a finished game');
  assert.equal(over.table_battles.length, 1, 'finished game keeps its table');
});
