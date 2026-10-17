// Vulnerability: the rearrange-hand endpoint reordered the caller's hand by an
// index list it only checked for length + range - not uniqueness. So
// card_indices:[0,0,0,0,0,0] passed, and `indices.map(i => hand[i])` produced six
// copies of one card (dropping the rest) - minting duplicate cards into the hand,
// persisted through the commit. A cheater could clone a trump ace six times.
//
// The rule is now the kernel's (c/src/game.c game_rearrange_hand behind
// table_rearrange_hand). Owns the rearrange validation scenario (DB-free: the
// fixtures' C Table in memory); the fast runner
// (e2e/validation/handlers_validation.test.ts) imports
// `registerRearrangeValidation`. The tests that drove the TypeScript server's meta
// handler through Postgres went with it (stage 4); c_base serves the same kernel
// call.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixture, fixtureTable, PLAYING } from './helpers/table_fixture.ts';
import { cardText, residentBoard, type PlayCard } from './helpers/table_play.ts';

const keys = (hand: PlayCard[]) => hand.map(cardText);

// ---- handpicked validation on the kernel (no DB) ------------------------------
export function registerRearrangeValidation(): void {
    const HERO = 'hero', OTHER = 'other';
    // A dealt two-seat board; the hero (seat 0) holds four distinct cards.
    const board = () => fixture()
        .seats([{ id: HERO, name: 'hero' }, { id: OTHER, name: 'other' }])
        .status(PLAYING).attacker(0).defender(1)
        .hand(0, '6c 7d 8h 9s').hand(1, 'Tc Jd Qh Ks').deck('Ac Ad').trump('Ah').discard(25)
        .build();
    const loaded = () => {
        const fx = board();
        const t = fixtureTable();
        assert.equal(t.load(fx.state, fx.roster), L.TABLE_OK, 'fixture loads');
        return { fx, t };
    };
    const heroHand = (fx: { state: Uint8Array; roster: Uint8Array }) => keys(residentBoard('v', fx.state, fx.roster).seats[0].hand);

    test('rearrange: duplicate indices ([0,0,...]) are rejected - no card cloning', () => {
        const { fx, t } = loaded();
        assert.equal(t.rearrangeHand(HERO, [0, 0, 0, 0]), L.TABLE_E_WIRE, 'duplicate indices are refused');
        assert.deepEqual(heroHand(fx), ['6c', '7d', '8h', '9s'], 'hand untouched by the refused call');
    });

    test('rearrange: a real permutation reorders; bad length / out of range / non-integer / non-member rejected', () => {
        const { fx, t } = loaded();
        assert.equal(t.rearrangeHand(HERO, [3, 2, 1, 0]), L.TABLE_OK);
        assert.deepEqual(heroHand(fx), ['9s', '8h', '7d', '6c'], 'the permutation landed on the hero');
        assert.deepEqual(keys(residentBoard('v', fx.state, fx.roster).seats[1].hand), ['Tc', 'Jd', 'Qh', 'Ks'], 'the other seat is untouched');
        assert.equal(t.rearrangeHand(HERO, [0, 1, 2]), L.TABLE_E_WIRE, 'wrong length');
        assert.equal(t.rearrangeHand(HERO, [4, 0, 1, 2]), L.TABLE_E_WIRE, 'out of range');
        assert.equal(t.rearrangeHand(HERO, [0.5, 1, 2, 3]), L.TABLE_E_WIRE, 'non-integer');
        assert.equal(t.rearrangeHand('unknown', [0, 1, 2, 3]), L.TABLE_E_NOT_SEATED, 'non-member');
        assert.deepEqual(heroHand(fx), ['9s', '8h', '7d', '6c'], 'refusals left the permuted hand as it was');
    });
}

// ---- full e2e: the REAL meta handler through the REAL CAS commit -------------

// The handpicked cases also run in the full e2e suite.
if (!process.env.VALIDATION_ONLY) registerRearrangeValidation();

