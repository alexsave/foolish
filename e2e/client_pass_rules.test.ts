// The pass, as the web plays it: the shield the optimistic board hands on and the
// gate every input path asks (src/utils/gameValidation.ts canPass) are the kernel's,
// on the board the screen holds.
//
//   1. The optimistic board of a pass hands the shield to the next seat still in
//      play (get_next_player_index), skipping a seat that is out.
//   2. canPass looks at that next defender's room for the table plus the passed card.
//
// Pure logic - no Postgres, no harness. The fast runner
// (e2e/validation/client_rules_validation.test.ts) imports
// `registerClientRulesValidation` and executes them.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { canPass } from '../src/utils/gameValidation.ts';
import { optimisticBoard } from '../src/state/clientBoards.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import type { TableView, ViewCard as Card } from '../sdk/ts/table/client_table.ts';
import { boardFixture, fixtureView, type BoardSpec } from './helpers/table_mem.ts';

// The shield the board passes to once the viewer's pass stands on it.
const nextDefenderIndex = (view: TableView): number | undefined =>
    optimisticBoard(view, encodeAction({ kind: 'pass', cards: [view.myHand[0] ?? { suit: 0, value: 6 }] }))?.defender;

interface Spec { status: 'in' | 'out'; hand_length: number }
const c = (suit: number, value: number): Card => ({ suit, value });

// The board the defender is shown: a kernel-sealed board read from the
// defender's envelope. The pass's rotation and canPass read the seats' statuses
// and counts, the defender and the table. The defender is the local player and
// must actually HOLD the cards it passes (the kernel checks hand membership -
// the old TS canPass did not); every other seat holds its count of cards
// nobody names.
function makeGame(defender: number, specs: Spec[], table: NonNullable<BoardSpec['table']>, selfCards: Card[] = []): TableView {
    return fixtureView(boardFixture({
        hands: specs.map((s, i) => (i === defender ? selfCards : s.hand_length)),
        out: specs.flatMap((s, i) => (s.status === 'out' ? [i] : [])),
        table, powerSuit: 0, attacker: (defender + specs.length - 1) % specs.length, defender,
    }), defender);
}

export function registerClientRulesValidation(): void {
    // ---- 1: the optimistic pass's shield ----------------------------------------
    test('a pass hands the shield past an eliminated seat', () => {
        // defender at seat 1; seat 2 is OUT; the next defender wraps to seat 0.
        const g = makeGame(1, [{ status: 'in', hand_length: 5 }, { status: 'in', hand_length: 4 }, { status: 'out', hand_length: 0 }], []);
        assert.equal(nextDefenderIndex(g), 0, 'skips the out seat to the next in-play player');
    });

    test('a pass hands the shield to the next seat when nobody is eliminated', () => {
        const g = makeGame(0, [{ status: 'in', hand_length: 5 }, { status: 'in', hand_length: 5 }, { status: 'in', hand_length: 5 }], []);
        assert.equal(nextDefenderIndex(g), 1);
    });

    // ---- 2: canPass and the next defender's room ---------------------------------
    test('canPass is FALSE when the next defender lacks room', () => {
        // Two uncovered 7s on the table; defender passes a third 7 -> next defender
        // would face 3 cards but holds only 1.
        const g = makeGame(0,
            [{ status: 'in', hand_length: 3 }, { status: 'in', hand_length: 1 }, { status: 'in', hand_length: 5 }],
            [{ attack: c(1, 7), defense: null }, { attack: c(2, 7), defense: null }], [c(0, 7)]);
        assert.equal(canPass(g, [c(0, 7)]), false, '2 on table + 1 passed = 3 > next defender hand of 1');
    });

    test('canPass is TRUE for a legal pass the keyboard should offer', () => {
        const g = makeGame(0,
            [{ status: 'in', hand_length: 3 }, { status: 'in', hand_length: 4 }, { status: 'in', hand_length: 5 }],
            [{ attack: c(1, 7), defense: null }], [c(0, 7)]);
        assert.equal(canPass(g, [c(0, 7)]), true);
    });

    test('canPass over an eliminated next seat checks the REAL next defender', () => {
        // defender seat 1, seat 2 OUT (0 cards), real next defender seat 0 has room.
        const g = makeGame(1,
            [{ status: 'in', hand_length: 5 }, { status: 'in', hand_length: 4 }, { status: 'out', hand_length: 0 }],
            [{ attack: c(3, 8), defense: null }, { attack: c(2, 8), defense: null }], [c(0, 8)]);
        assert.equal(canPass(g, [c(0, 8)]), true, 'must look past the out seat to seat 0 (room for 3)');
    });
}

if (!process.env.VALIDATION_ONLY) registerClientRulesValidation();
