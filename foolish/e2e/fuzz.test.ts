// Always-reject invariants of the C Table's table_act on kernel-built boards,
// in memory: a forged card, an identical duplicate and a non-member attack are
// refused for the named reason and leave the hand untouched.
// registerAttackValidation also runs in e2e/validation/handlers_validation.test.ts.
//
// The adversarial fuzzer that sent hostile request bodies through the
// TypeScript server's move path against Postgres went with that server
// (stage 4 of c_base/DESIGN.md "Binary surface"); c_base's unit_app
// (the_wire_surface, an_illegal_action_is_rejected_and_commits_nothing) and
// e2e/wasm_kernel_fuzz.test.ts hold those invariants now.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { AWIRE_KIND } from '../sdk/ts/table/client_table.ts';
import { fixture, fixtureTable } from './helpers/table_fixture.ts';
import { residentBoard, type PlayCard } from './helpers/table_play.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

// A card's wire byte, named here ON PURPOSE: this file forges frames the kernel's
// encoder refuses to write (cards on a pickup, counts the bytes do not carry),
// so it packs its own bytes rather than ask the encoder it is attacking around.
const wc = (c: PlayCard) => c.suit * 13 + (c.value - 1);

// ---- handpicked, pure validation (no DB): the always-reject invariants -------
export function registerAttackValidation(): void {
    test('attack: forged card, identical-duplicate, and non-member attacks are all rejected', () => {
        // The kernel builds the board: atk (seat 0) attacks def (seat 1).
        const fx = fixture().seats([{ id: 'atk', name: 'atk' }, { id: 'def', name: 'def' }])
            .status(L.GAME_STATUS_PLAYING).attacker(0).defender(1).powerSuit(0)
            .hand(0, '6c 6d 8h').hand(1, 'Ks').deck('').discard(32).build();
        const table = fixtureTable();
        const hand0 = (): PlayCard[] => { table.load(fx.state, fx.roster); return residentBoard('g', fx.state, fx.roster).seats[0].hand; };
        const original = hand0();
        assert.equal(original.length, 3, 'fixture: the attacker holds three cards');

        const attack = (actor: string, cards: PlayCard[]) => {
            assert.equal(table.load(fx.state, fx.roster), L.TABLE_OK);
            const rc = table.act(actor, Uint8Array.from([AWIRE_KIND.attack, cards.length, ...cards.map(wc)]), null, 0);
            return { rc, reject: rc === L.TABLE_REJECTED ? table.reject() : 0, hand: residentBoard('g', fx.state, fx.roster).seats[0].hand };
        };

        // forged: a card the attacker does not hold
        const forged = attack('atk', [{ suit: 3, value: 9 }]);
        assert.equal(forged.rc, L.TABLE_REJECTED, 'forged card');
        assert.equal(forged.reject, L.ENGINE_REJECT_NOT_IN_HAND, 'forged card: not in hand');
        assert.deepEqual(forged.hand, original, 'forged card: the hand is untouched');
        // the object-identity duplicate hole: [X, X] must be rejected, never duplicated
        const dup = attack('atk', [original[0], original[0]]);
        assert.equal(dup.rc, L.TABLE_REJECTED, 'identical duplicate');
        assert.equal(dup.reject, L.ENGINE_REJECT_DUPLICATES, 'identical duplicate: duplicates');
        assert.deepEqual(dup.hand, original, 'identical duplicate: the hand is untouched');
        // a player who isn't in the game
        assert.equal(attack('ghost', [original[0]]).rc, L.TABLE_E_NOT_SEATED, 'non-member');
        assert.equal(attack('', [original[0]]).rc, L.TABLE_E_NOT_SEATED, 'empty actor id');
        assert.equal(attack('at', [original[0]]).rc, L.TABLE_E_NOT_SEATED, 'a prefix of a seated id');
    });
}

// The handpicked cases also run in the full e2e suite.
if (!process.env.VALIDATION_ONLY) registerAttackValidation();

