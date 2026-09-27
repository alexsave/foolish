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
// `registerRearrangeValidation`. The full e2e tests drive the REAL meta handler
// through the REAL CAS commit (commit_table).

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixture, fixtureTable, PLAYING } from './helpers/table_fixture.ts';
import { cardText, checkCardConservation, mustReadTable, residentBoard, type PlayCard } from './helpers/table_play.ts';
import { runMeta, seedLobby } from './helpers/table_server.ts';
import { __setTableDealSeedOverride } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';

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
if (!process.env.VALIDATION_ONLY) {
    before(async () => {
        await applySchema();
        // A pinned deal, so a failure reproduces.
        __setTableDealSeedOverride(new Uint8Array(32).fill(0x5a));
    });
    beforeEach(async () => { await resetDb(); __clearGameCache(); });

    async function freshGame(): Promise<{ gameId: string; human: string }> {
        const gameId = `rh${uuid().slice(0, 5)}`;
        const human = uuid();
        await seedLobby(gameId, [{ id: human, name: 'H', ready: false }, { id: uuid(), name: 'B', brain: 'random' }]);
        await runMeta(gameId, human, { type: 'start' });
        assert.equal((await mustReadTable(gameId)).status, L.GAME_STATUS_PLAYING, 'fixture: the game dealt');
        return { gameId, human };
    }
    const seatOf = (t: { seats: { id: string }[] }, id: string) => t.seats.findIndex((s) => s.id === id);

    test('rearrange-hand: duplicate indices ([0,0,...]) are rejected - no card duplication', async () => {
        const { gameId, human } = await freshGame();
        const before = await mustReadTable(gameId);
        const hand = before.seats[seatOf(before, human)].hand;
        assert.ok(hand.length > 0, 'human has a hand');

        const dupIndices = new Array(hand.length).fill(0);
        await assert.rejects(runMeta(gameId, human, { type: 'rearrange-hand', card_indices: dupIndices }),
            /malformed request/i, 'duplicate indices must be rejected');
        const after = await mustReadTable(gameId);
        assert.equal(after.version, before.version, 'the refusal committed nothing');
        assert.deepEqual(keys(after.seats[seatOf(after, human)].hand), keys(hand), 'the hand is as it was');
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `state intact after rejected exploit: ${chk.detail}`);

        // What the guard stops: the unguarded map clones one card into every slot.
        const naive = dupIndices.map((i) => hand[i]);
        assert.equal(new Set(keys(naive)).size, 1, 'unguarded map yields copies of one card');
    });

    test('rearrange-hand: a real permutation reorders the hand and conserves cards', async () => {
        const { gameId, human } = await freshGame();
        const before = await mustReadTable(gameId);
        const hand = before.seats[seatOf(before, human)].hand;
        const reversed = hand.map((_, i) => hand.length - 1 - i);

        await runMeta(gameId, human, { type: 'rearrange-hand', card_indices: reversed });

        const after = await mustReadTable(gameId);
        assert.equal(after.version, before.version + 1, 'committed');
        assert.deepEqual(keys(after.seats[seatOf(after, human)].hand), keys([...hand].reverse()), 'hand reversed');
        const chk = await checkCardConservation(gameId);
        assert.ok(chk.ok, `conserved after legit rearrange: ${chk.detail}`);
    });

    test('rearrange-hand: out-of-range / wrong-length / non-array indices and a non-member are rejected', async () => {
        const { gameId, human } = await freshGame();
        const before = await mustReadTable(gameId);
        const n = before.seats[seatOf(before, human)].hand.length;
        const identity = Array.from({ length: n }, (_, i) => i);
        await assert.rejects(runMeta(gameId, human, { type: 'rearrange-hand', card_indices: [n, 0, 1] }), /malformed request/i, 'out of range + wrong length');
        await assert.rejects(runMeta(gameId, human, { type: 'rearrange-hand', card_indices: [n, ...identity.slice(1)] }), /malformed request/i, 'out of range');
        await assert.rejects(runMeta(gameId, human, { type: 'rearrange-hand', card_indices: 'nope' }), /malformed request/i, 'non-array');
        await assert.rejects(runMeta(gameId, uuid(), { type: 'rearrange-hand', card_indices: identity }), /not in game/i, 'non-member');
        assert.equal((await mustReadTable(gameId)).version, before.version, 'no refusal committed');
    });

    registerRearrangeValidation();

    after(async () => { __setTableDealSeedOverride(null); await pgPool.end(); });
}
