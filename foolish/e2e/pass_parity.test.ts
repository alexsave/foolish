// Pass legality - the handpicked validation scenarios for passing (the fuzzer that
// played dealt games through the TypeScript server's Postgres move path went with
// that server in stage 4). Per project convention, anything pass-related lives in
// this file; the fast validation runner (e2e/validation/pass_parity_validation.test.ts)
// imports `registerPassValidation` and executes just those deterministic cases.
//
// Three independent oracles decide whether the CURRENT DEFENDER may pass a set of
// same-valued cards:
//   1. ground truth - the kernel's legal-move enumerator (legal.c, what the bots
//      and the client's move lists play), read through e2e/helpers/table_play.ts
//   2. SERVER       - the C Table's table_act (the operation the move path runs
//      for the auth id's seat): TABLE_APPLIED == legal, TABLE_REJECTED == illegal
//   3. CLIENT       - the Pass button from src/utils/gameValidation.ts (boardPills),
//      over the board (TableView) the client reads from the server's envelope bytes
//      (readEnvelopeView, the web's reader): the stored player_views row for a human defender
// The invariant: all three must agree for the defender's own hand. A disagreement
// is the "I could pass legally but the client gave me no option" bug (or its dual).
//
// Migrated to the C Table (docs/C_GAME_SHAPE_MIGRATION.md Phase 4b): the boards are
// kernel-built (e2e/helpers/table_fixture.ts) or kernel-dealt and played through the
// real move path; the TS twins the oracles used to be (calculateLegalMoves,
// actions/pass.ts validatePass, personalize_game) are no longer consulted. The
// SERVER oracle runs table_act in memory rather than through the endpoint because
// asking it commits: a probe that applied would move the game on.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import type { TableView } from '../sdk/ts/table/client_table.ts';
import { boardPills } from '../src/utils/gameValidation.ts';
import { PLAY_PILL_PASS } from '../sdk/ts/gen/view_layout.bots.ts';
import { readEnvelopeView as readEnvelope } from './helpers/client_read.ts';
import { encodeAction } from '../sdk/ts/table/client_table.ts';
import { fixture, fixtureTable, OUT } from './helpers/table_fixture.ts';
import { cardText, legalMoves, residentBoard, type BoardState, type PlayCard } from './helpers/table_play.ts';
import { suiteRng } from './helpers/rng.ts';

// Whether the board shows the Pass button for the selection `cards`.
const clientCanPass = (view: TableView, cards: readonly PlayCard[]): boolean => (boardPills(view, cards) & PLAY_PILL_PASS) !== 0;

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

type Card = PlayCard;

const cardKey = (c: Card) => `${c.suit}:${c.value}`;
const setKey = (cards: Card[]) => cards.map(cardKey).sort().join('|');

/** The kernel's legal passes for `seat` on the board, as set keys. */
function truthPasses(b: BoardState, seat: number): Set<string> {
    return new Set(legalMoves(b, (_, i) => i === seat).filter((m) => m.kind === 'pass').map((m) => setKey(m.cards)));
}

/** Does the C Table apply this pass by `seat`? (Loads the board first: an apply mutates the resident table.) */
function serverAllowsPass(b: BoardState, seat: number, cards: Card[]): boolean {
    const table = fixtureTable();
    assert.equal(table.load(b.state, b.roster), L.TABLE_OK, 'the board loads');
    const rc = table.act(b.seats[seat].id, encodeAction({ kind: 'pass', cards }), null, 0);
    assert.ok(rc === L.TABLE_APPLIED || rc === L.TABLE_REJECTED, `a pass is applied or rejected, got ${rc}`);
    return rc === L.TABLE_APPLIED;
}

/** The envelope the server writes for `seat` at `version`, as the client decodes it. */
function clientGame(b: BoardState, seat: number, version: number): TableView {
    const table = fixtureTable();
    assert.equal(table.load(b.state, b.roster), L.TABLE_OK, 'the board loads');
    const env = table.envelope(b.gameId, seat, version);
    assert.ok(typeof env !== 'number', `the envelope builds (${env})`);
    return readView(env);
}

function readView(bytes: Uint8Array): TableView {
    const d = readEnvelope(bytes);
    assert.ok(d, 'the client decodes the envelope');
    return d;
}

// ===========================================================================
// Handpicked, deterministic validation scenarios (no fuzzing, no DB). Exported
// so the fast validation runner can execute exactly these.
// ===========================================================================
export function registerPassValidation(): void {
    const c = (suit: number, value: number): Card => ({ suit, value });
    interface PlayerSpec { status: 'in' | 'out'; hand: Card[] }
    const text = (cards: Card[]) => cards.map((x) => cardText(x)).join(' ');

    // A PLAYING board built and sealed by the kernel. The deck is out, so the
    // discard pile holds every card not in a hand or on the table.
    const makeBoard = (defender: number, players: PlayerSpec[], table: { attack: Card; defense: Card | null }[], powerSuit = 0): BoardState => {
        const out = players.flatMap((p, i) => (p.status === 'out' ? [i] : []));
        const live = players.reduce((n, p) => n + p.hand.length, 0) + table.reduce((n, b) => n + (b.defense ? 2 : 1), 0);
        let f = fixture().seats(players.map((_, i) => ({ id: `P${i}`, name: `P${i}` })))
            .status(L.GAME_STATUS_PLAYING).powerSuit(powerSuit)
            .attacker((defender + players.length - 1) % players.length).defender(defender)
            .table(...table.map((b) => (b.defense ? `${text([b.attack])}/${text([b.defense])}` : text([b.attack]))))
            .eliminated(...out).discard(36 - live);
        players.forEach((p, i) => { f = f.hand(i, text(p.hand)); if (p.status === 'out') f = f.seatStatus(i, OUT); });
        const fx = f.build();
        assert.equal(fixtureTable().load(fx.state, fx.roster), L.TABLE_OK);
        return residentBoard('g', fx.state, fx.roster);
    };

    // All three oracles must agree with `expected` for this pass.
    const expectParity = (name: string, b: BoardState, defender: number, cards: Card[], expected: boolean) => {
        const s = serverAllowsPass(b, defender, cards);
        const t = truthPasses(b, defender).has(setKey(cards));
        const cl = clientCanPass(clientGame(b, defender, 1), cards);
        assert.equal(s, expected, `${name}: SERVER legality ${s} !== expected ${expected}`);
        assert.equal(t, expected, `${name}: GROUND-TRUTH legality ${t} !== expected ${expected}`);
        assert.equal(cl, expected, `${name}: CLIENT legality ${cl} !== expected ${expected} (client/server disagree)`);
    };

    // THE REGRESSION: defender at seat 1, seat 2 is OUT (empty hand), the real next
    // defender (wrapping past the out seat to seat 0) has room. Passing a third
    // card of the value is legal - the client must not look at the out seat's
    // empty hand and hide it.
    test('pass parity: legal pass when the seat after the defender is eliminated (the reported bug)', () => {
        const b = makeBoard(1, [
            { status: 'in', hand: [c(0, 5), c(1, 6), c(2, 9), c(3, 10), c(0, 11)] }, // P0 real next defender, 5 cards
            { status: 'in', hand: [c(0, 8), c(1, 12), c(2, 13), c(3, 13)] },          // P1 defender, holds the value
            { status: 'out', hand: [] },                                              // P2 eliminated
        ], [{ attack: c(3, 8), defense: null }, { attack: c(2, 8), defense: null }]);
        expectParity('out-seat-after-defender', b, 1, [c(0, 8)], true);
    });

    test('pass parity: blocked when the real next defender (past an out seat) lacks room', () => {
        const b = makeBoard(1, [
            { status: 'in', hand: [c(0, 5), c(1, 6)] },                       // P0 real next defender, only 2 cards
            { status: 'in', hand: [c(0, 8), c(1, 12), c(2, 13), c(3, 13)] },  // P1 defender
            { status: 'out', hand: [] },                                      // P2 eliminated
        ], [{ attack: c(3, 8), defense: null }, { attack: c(2, 8), defense: null }]); // 2 + 1 = 3 > 2
        expectParity('out-seat-real-next-too-small', b, 1, [c(0, 8)], false);
    });

    test('pass parity: ordinary legal pass with all players in', () => {
        const b = makeBoard(0, [
            { status: 'in', hand: [c(0, 7), c(1, 12), c(2, 13)] },            // P0 defender, holds the value
            { status: 'in', hand: [c(0, 5), c(1, 6), c(2, 9), c(3, 10)] },    // P1 next defender, 4 cards
            { status: 'in', hand: [c(0, 11), c(2, 11)] },                     // P2
        ], [{ attack: c(1, 7), defense: null }]);
        expectParity('ordinary-legal', b, 0, [c(0, 7)], true);
    });

    test('pass parity: ordinary pass blocked when next defender lacks room', () => {
        const b = makeBoard(0, [
            { status: 'in', hand: [c(0, 7), c(1, 7), c(2, 13)] },             // P0 defender, two of the value
            { status: 'in', hand: [c(0, 5)] },                               // P1 next defender, 1 card
            { status: 'in', hand: [c(0, 11), c(2, 11)] },                     // P2
        ], [{ attack: c(3, 7), defense: null }, { attack: c(2, 7), defense: null }]); // 2 + 1 = 3 > 1
        expectParity('ordinary-blocked', b, 0, [c(0, 7)], false);
    });

    test('pass parity: cannot pass once a battle is covered', () => {
        const b = makeBoard(0, [
            { status: 'in', hand: [c(0, 7), c(1, 12)] },                      // P0 defender, holds the value
            { status: 'in', hand: [c(0, 5), c(1, 6), c(2, 9), c(3, 10)] },    // P1 next defender
            { status: 'in', hand: [c(0, 11), c(2, 11)] },                     // P2
        ], [{ attack: c(1, 7), defense: c(1, 13) }]); // already covered
        expectParity('covered-no-pass', b, 0, [c(0, 7)], false);
    });
}

// ===========================================================================
// Exhaustive fuzzer + its validation cases - only when run directly (full e2e),
// NOT when the validation runner imports this file for registerPassValidation.
// ===========================================================================

// The handpicked cases also run in the full e2e suite.
if (!process.env.VALIDATION_ONLY) registerPassValidation();

