// Pass legality - owns BOTH the exhaustive fuzzer AND the handpicked validation
// scenarios for passing. Per project convention, anything pass-related lives in
// this file; the fast validation runner (e2e/validation/pass_parity_validation.test.ts)
// imports `registerPassValidation` and executes just those deterministic cases.
//
// Three independent oracles decide whether the CURRENT DEFENDER may pass a set of
// same-valued cards:
//   1. ground truth - the kernel's legal-move enumerator (legal.c, what the bots
//      and the client's move lists play), read through e2e/helpers/table_play.ts
//   2. SERVER       - the C Table's table_act (the operation the move path runs
//      for the auth id's seat): TABLE_APPLIED == legal, TABLE_REJECTED == illegal
//   3. CLIENT       - canPass from src/utils/gameValidation.ts (the UI button gate),
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

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, uuid, pgPool } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import type { TableView } from '../sdk/ts/table/client_table.ts';
import { canPass as clientCanPass } from '../src/utils/gameValidation.ts';
import { readEnvelopeView as readEnvelope } from './helpers/client_read.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import { __setTableDealSeedOverride } from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { fixture, fixtureTable, OUT } from './helpers/table_fixture.ts';
import { cardText, legalMoves, mustReadTable, residentBoard, type BoardState, type PlayCard } from './helpers/table_play.ts';
import { runAction, runMeta, seedLobby } from './helpers/table_server.ts';
import { suiteRng } from './helpers/rng.ts';

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
if (!process.env.VALIDATION_ONLY) {
    // Every non-empty same-value subset of the defender's hand whose value also
    // appears on the table - the candidate space we ask all three oracles about.
    const candidatePassSets = (hand: PlayCard[], b: BoardState): Card[][] => {
        if (b.battles.length === 0) return [];
        const tableValues = new Set(b.battles.map((x) => x.attack.value));
        const byValue = new Map<number, Card[]>();
        for (const c of hand) {
            if (!tableValues.has(c.value)) continue;
            (byValue.get(c.value) ?? byValue.set(c.value, []).get(c.value)!).push(c);
        }
        const out: Card[][] = [];
        for (const cards of byValue.values()) {
            const n = Math.min(cards.length, 6);
            for (let mask = 1; mask < (1 << n); mask++) {
                const set: Card[] = [];
                for (let i = 0; i < n; i++) if (mask & (1 << i)) set.push(cards[i]);
                out.push(set);
            }
        }
        return out;
    };

    // Moves and deals from the suite seed (E2E_SEED_PASS_PARITY), so a red run replays.
    const rng = suiteRng('pass_parity');

    const freshGame = async (): Promise<string> => {
        const gameId = `p${uuid().slice(0, 6)}`;
        const h0 = uuid();
        // Three players so the game eliminates someone mid-play, producing the
        // OUT-player-between-defender-and-next states the bug needs.
        await seedLobby(gameId, [
            { id: h0, name: 'H0', ready: false },
            { id: uuid(), name: 'H1' },
            { id: uuid(), name: 'B0', brain: 'random' },
        ]);
        __setTableDealSeedOverride(Uint8Array.from({ length: 32 }, () => rng.int(256)));
        await runMeta(gameId, h0, { type: 'start' });
        return gameId;
    };

    before(async () => { await applySchema(); });
    beforeEach(async () => { await resetDb(); __clearGameCache(); });
    after(async () => { __setTableDealSeedOverride(null); await pgPool.end(); });

    test('pass parity fuzz: client offers exactly the legal passes the server accepts', async () => {
        const ITER = Number(process.env.FUZZ_ITERS || 8000);
        let gameId = await freshGame();
        let checkedStates = 0, sawEliminations = 0, fromStoredView = 0;
        const hiddenLegal: string[] = [];    // server YES, client NO (the reported bug)
        const offeredIllegal: string[] = []; // client YES, server NO (the dual)
        const truthDisagrees: string[] = []; // the enumerator and the table disagree

        for (let i = 0; i < ITER; i++) {
            let t = await mustReadTable(gameId);
            if (t.status !== L.GAME_STATUS_PLAYING) { gameId = await freshGame(); t = await mustReadTable(gameId); }

            const d = t.defender;
            const defender = t.seats[d];
            if (defender && defender.status === L.PLAYER_STATUS_IN && t.battles.length > 0) {
                if (t.seats.some((s) => s.status === L.PLAYER_STATUS_OUT)) sawEliminations++;
                // The client's game: the player_views row the server committed for a
                // human defender (what the client reads), the same envelope in memory
                // for the bot seat (which has no client and no row).
                let personal: TableView;
                const row = defender.brain ? null : (await pgPool.query(
                    'SELECT view, version FROM player_views WHERE game_id=$1 AND player_id=$2', [gameId, defender.id])).rows[0];
                if (row) {
                    assert.equal(Number(row.version), t.version, `the stored view is at the row's version (iter=${i})`);
                    personal = readView(Buffer.from(row.view.replace(/^\\x/, ''), 'hex'));
                    fromStoredView++;
                } else {
                    personal = clientGame(t, d, t.version);
                }
                const truthKeys = truthPasses(t, d);

                for (const cards of candidatePassSets(defender.hand, t)) {
                    const k = setKey(cards);
                    const truthYes = truthKeys.has(k);
                    const serverYes = serverAllowsPass(t, d, cards);
                    const clientYes = clientCanPass(personal, cards);
                    checkedStates++;
                    const detail = `seed=${rng.seed} iter=${i} truth=${truthYes} cards=[${cards.map(cardKey).join(',')}] `
                        + `defender=#${d}(${defender.name}) players=[${t.seats.map((s, idx) => `${idx}:${s.name}:${s.status}:h${s.hand.length}`).join(' ')}] `
                        + `table=[${t.battles.map((b) => `${cardKey(b.attack)}${b.defense ? '/' + cardKey(b.defense) : ''}`).join(' ')}]`;
                    if (serverYes && !clientYes && hiddenLegal.length < 3) hiddenLegal.push(detail);
                    if (clientYes && !serverYes && offeredIllegal.length < 3) offeredIllegal.push(detail);
                    if (truthYes !== serverYes && truthDisagrees.length < 3) truthDisagrees.push(detail);
                }
            }

            const moves = legalMoves(t);
            if (moves.length) {
                const m = rng.pick(moves);
                try { await runAction(gameId, m.playerId, m); } catch { /* race -> no-op */ }
            } else { gameId = await freshGame(); }
        }

        process.stdout.write(`[pass-parity] checked=${checkedStates} statesWithEliminations=${sawEliminations} fromStoredView=${fromStoredView} hiddenLegal=${hiddenLegal.length} offeredIllegal=${offeredIllegal.length} truthDisagrees=${truthDisagrees.length}\n`);
        assert.ok(checkedStates > 0 && sawEliminations > 0 && fromStoredView > 0,
            `the fuzz reached pass states, eliminations and stored views (seed=${rng.seed})`);
        assert.equal(hiddenLegal.length, 0, `CLIENT hides a pass the SERVER accepts (legal pass, no button):\n  ${hiddenLegal.join('\n  ')}`);
        assert.equal(offeredIllegal.length, 0, `CLIENT offers a pass the SERVER rejects:\n  ${offeredIllegal.join('\n  ')}`);
        assert.equal(truthDisagrees.length, 0, `the legal-move enumerator and the SERVER disagree on a pass:\n  ${truthDisagrees.join('\n  ')}`);
    });

    // The handpicked cases also run as part of the full e2e suite.
    registerPassValidation();
}
