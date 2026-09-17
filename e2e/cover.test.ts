// The cover move on the C Table (table_act, the move path the server runs): the
// validate/execute matching-mismatch fix. Pure kernel, no DB needed.
//
// It used to drive the TS handler (server/api/common/actions/cover.ts handleCover)
// on a TypeScript Game; that twin is gone with the TS game shape
// (docs/C_GAME_SHAPE_MIGRATION.md Phase 8), and the same double tap is asked of
// the kernel on a board it built (e2e/helpers/table_fixture.ts).
//
// Owns the cover validation scenarios; the fast runner
// (e2e/validation/handlers_validation.test.ts) imports `registerCoverValidation`.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import { boardFixture, MemTable } from './helpers/table_mem.ts';

const card = (suit: number, value: number) => ({ suit, value });
// Two uncovered sixes (spades, hearts), spades trump; the defender holds the 7 and 8 of spades.
const makeTable = (): MemTable => MemTable.of(boardFixture({
    hands: [4, [card(0, 7), card(0, 8)]],
    table: [{ attack: card(0, 6), defense: null }, { attack: card(1, 6), defense: null }],
    powerSuit: L.SUIT_SPADES, attacker: 0, defender: 1,
}));
const cover = (c: { suit: number; value: number }, a: { suit: number; value: number }) =>
    encodeAction({ kind: 'cover', cards: [c], attack_cards: [a] });

export function registerCoverValidation(): void {
    test('cover: double-tapping an already-covered same-rank attack is refused cleanly, not a server error', () => {
        const t = makeTable();
        assert.equal(t.act(1, cover(card(0, 7), card(0, 6))).rc, L.TABLE_APPLIED, 'cover the 6 of spades');
        // The 6 of spades is covered; the 6 of hearts is still uncovered.
        const again = t.act(1, cover(card(0, 8), card(0, 6)));
        assert.equal(again.rc, L.TABLE_REJECTED, 'must be a refusal the client can show, not a throw');
        assert.equal(again.reject, L.ENGINE_REJECT_ATTACK_NOT_ON_TABLE, 'the attack it names is not on the table uncovered');
    });

    test('cover: the still-uncovered same-rank attack can be covered', () => {
        const t = makeTable();
        assert.equal(t.act(1, cover(card(0, 7), card(0, 6))).rc, L.TABLE_APPLIED, 'cover the 6 of spades');
        assert.equal(t.act(1, cover(card(0, 8), card(1, 6))).rc, L.TABLE_APPLIED, 'cover the 6 of hearts');
    });
}

if (!process.env.VALIDATION_ONLY) registerCoverValidation();
