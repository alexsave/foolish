// src/utils/coverCombinations.ts is the shared multi-card cover resolver behind
// both the drag (DragContext) and keyboard (KeyboardInputHandler) cover paths:
// it enumerates every legal cover->attack pairing and decides whether the
// selection covers an unambiguous set of attacks. Pure — no DOM, no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { findCoverCombinations, findUnambiguousCover } from '../src/utils/coverCombinations.ts';
import { Card, Battle } from '../supabase/functions/_shared/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const C = (suit: number, value: number): Card => ({ suit, value });
const POWER = 3; // diamonds trump
const attackSet = (combo: { attackCards: Card[] }) => new Set(combo.attackCards.map(c => `${c.value}-${c.suit}`));

test('findCoverCombinations returns nothing for empty inputs or too-many covers', () => {
  assert.deepEqual(findCoverCombinations([], [C(0, 7)], POWER), [], 'no cover cards');
  assert.deepEqual(findCoverCombinations([C(0, 9)], [], POWER), [], 'no attacks');
  assert.deepEqual(findCoverCombinations([C(0, 9), C(0, 10)], [C(0, 7)], POWER), [], 'more covers than attacks');
});

test('findCoverCombinations enumerates only the legal cover->attack pairings', () => {
  // 9♠ covers only 7♠, 9♣ covers only 8♣ -> exactly one legal pairing.
  const one = findCoverCombinations([C(0, 9), C(2, 9)], [C(0, 7), C(2, 8)], POWER);
  assert.equal(one.length, 1, 'a single legal assignment');
  assert.deepEqual(attackSet(one[0]), new Set(['7-0', '8-2']));

  // A single cover with two beatable attacks -> two legal pairings.
  const two = findCoverCombinations([C(0, 10)], [C(0, 7), C(0, 8)], POWER);
  assert.equal(two.length, 2, '10♠ can cover either 7♠ or 8♠');

  // A cover that beats nothing -> no combinations.
  assert.deepEqual(findCoverCombinations([C(0, 3)], [C(0, 7)], POWER), [], 'cannot beat the attack');
});

test('findUnambiguousCover returns a mapping only when the covered set is unique', () => {
  // Unambiguous: each cover fits exactly one attack.
  const table1: Battle[] = [{ attack: C(0, 7), defense: null }, { attack: C(2, 8), defense: null }];
  const unambiguous = findUnambiguousCover([C(0, 9), C(2, 9)], table1, POWER);
  assert.ok(unambiguous, 'a unique covered set resolves');
  assert.deepEqual(attackSet(unambiguous!), new Set(['7-0', '8-2']));

  // Ambiguous: two trumps over three attacks -> which two get covered is unclear.
  const table2: Battle[] = [
    { attack: C(0, 7), defense: null }, { attack: C(0, 8), defense: null }, { attack: C(0, 9), defense: null },
  ];
  assert.equal(findUnambiguousCover([C(3, 10), C(3, 11)], table2, POWER), null, 'ambiguous -> null');

  // No legal cover at all -> null.
  assert.equal(findUnambiguousCover([C(0, 3)], [{ attack: C(0, 7), defense: null }], POWER), null, 'no cover -> null');

  // Covered battles are ignored when choosing targets.
  const table3: Battle[] = [{ attack: C(0, 7), defense: C(0, 12) }, { attack: C(2, 8), defense: null }];
  const only8 = findUnambiguousCover([C(2, 9)], table3, POWER);
  assert.ok(only8 && attackSet(only8).has('8-2') && !attackSet(only8).has('7-0'), 'only the uncovered attack is targeted');
});
