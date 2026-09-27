// Masked views: what the server writes for a viewer is the committed board,
// masked for that viewer, and nothing more.
//
// For real kernel-driven games: the stored row -> table_envelope(viewer) (the
// kernel's per-viewer masking and the response framing) -> the client slot's read
// (readEnvelopeView, the web's reader). The board a viewer reads must equal the
// committed board, read in full through the generated accessors
// (e2e/helpers/table_play.ts residentBoard), on every public field, with the
// viewer's own hand exact and in order, for every seat and for the spectator.
//
// The JSON-era comparison with personalize_game (a TS twin of the masking,
// deleted with the TS game shape, plan Q11) and the TS envelope encoder are gone:
// the envelope is the kernel's own, and S1 (security_hidden_info) scans every
// payload the server really sends.
//
// The raw view must also never carry another player's hand identities. That is
// asserted where it lives, on the bytes ("a masked view does not depend on the
// hands it is masking" below): a reader cannot show it, since a view the client
// reads holds no hand but the viewer's own by construction.
//
// Pure kernel test - needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { legalMoves, rebuild, residentBoard, type BoardState, type PlayCard } from './helpers/table_play.ts';
import { readEnvelopeView } from './helpers/client_read.ts';
import { dealTable, lcg, tableMove } from './helpers/kernel_board.ts';
import { seedBytes, type BotTableRow } from './helpers/bot_table.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const rnd = lcg(Number(process.env.FUZZ_SEED || 0x5eed1e55));
const ri = (n: number) => Math.floor(rnd() * n);
const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');

// Humans at even seats, bots at odd ones, as the codec's mixed tables were.
const seatsFor = (np: number) => Array.from({ length: np }, (_, i) => ({ id: `player-${i}`, name: `P${i}`, brain: i % 2 === 1 ? 'random' : '' }));

function boardOf(row: { gameId: string; state: Uint8Array; roster: Uint8Array }): BoardState {
    assert.equal(fixtureTable().load(row.state, row.roster), L.TABLE_OK, 'the row loads');
    return residentBoard(row.gameId, row.state, row.roster);
}

function envelope(row: { gameId: string; state: Uint8Array; roster: Uint8Array }, viewer: number, version: number): Uint8Array {
    const table = fixtureTable();
    assert.equal(table.load(row.state, row.roster), L.TABLE_OK, 'the row loads');
    const env = table.envelope(row.gameId, viewer, version);
    assert.ok(env instanceof Uint8Array, `the envelope builds (${env})`);
    return env;
}

const card = (c: PlayCard) => ({ suit: c.suit, value: c.value });

// One viewer of one committed state: the read board is the committed board.
function checkView(row: BotTableRow, b: BoardState, seat: number, tag: string): void {
    const v = readEnvelopeView(envelope(row, seat, row.version));
    assert.ok(v, `${tag}: the envelope reads`);
    assert.equal(v.version, row.version, `${tag}: version survives the envelope`);
    assert.equal(v.mySeat, seat, `${tag}: seat survives the envelope`);
    assert.equal(v.gameId, row.gameId, `${tag}: game id`);
    assert.equal(v.status, b.status, `${tag}: status`);
    assert.deepEqual(v.seats.map((s) => [s.id, s.name, s.isAi, s.status, s.handCount]),
        b.seats.map((s) => [s.id, s.name, s.brain !== '', s.status, s.hand.length]), `${tag}: public seats (names/status/hand counts)`);
    assert.deepEqual(v.battles.map((x) => [card(x.attack), x.defense.value > 0 ? card(x.defense) : null]),
        b.battles.map((x) => [card(x.attack), x.defense ? card(x.defense) : null]), `${tag}: table battles`);
    assert.equal(v.deckCount, b.deckCount, `${tag}: deck count`);
    assert.equal(v.discardPileLength, b.discard, `${tag}: discard`);
    assert.equal(v.hasFlipped, b.trump !== null, `${tag}: flipped`);
    if (b.trump) assert.deepEqual(card(v.flipped), card(b.trump), `${tag}: the face-up trump`);
    assert.equal(v.powerSuit, b.powerSuit, `${tag}: power suit`);
    assert.equal(v.firstAttacker, b.firstAttacker, `${tag}: first attacker`);
    assert.equal(v.defender, b.defender, `${tag}: defender`);
    assert.equal(v.goodMask, b.goodMask, `${tag}: goods said`);
    assert.equal(v.hasGoodTimestamp, b.hasGoodTimestamp, `${tag}: good clock running`);
    assert.deepEqual([...v.elimination], b.eliminated, `${tag}: elimination order`);
    if (seat >= 0) {
        assert.deepEqual(v.myHand.map(card), b.seats[seat].hand.map(card), `${tag}: own hand identical, in order`);
        assert.equal(v.seats[seat].awaitingAttack, b.seats[seat].awaiting, `${tag}: awaiting_attack real for the viewer`);
    } else {
        assert.equal(v.myHand.length, 0, `${tag}: a spectator holds no hand`);
    }
}

// ---------------------------------------------------------------------------
// THE MASKING ITSELF, ON THE BYTES
// ---------------------------------------------------------------------------
// A MASKED VIEW MUST NOT DEPEND ON WHAT IT IS MASKING. Change a hand the viewer
// cannot see, and the bytes it receives must be identical. Any leak - ordered,
// unordered, partial - changes them. (Measured once, with state_put's masked-hand
// memset replaced by the real cards: twelve of the other seats' card ids in a
// seat-0 view, and every reader-level suite still green.)
test('a masked view does not depend on the hands it is masking', () => {
    let compared = 0;
    for (let g = 0; g < 6; g++) {
        const np = 2 + (g % 5);
        const row = dealTable(seatsFor(np), seedBytes(np, 7919 * g + 3), { gameId: 'viewgame' });
        const b = boardOf(row);

        for (let viewer = -1; viewer < np; viewer++) {
            const before = envelope(row, viewer, 1);

            // Rewrite every hand the viewer may NOT see, keeping the counts (a count
            // is public and legitimately in the view). The new cards are the same
            // cards moved around - every hidden hand plus the stock (masked in every
            // view), rotated by one - so the kernel still accepts the board.
            const pool = [...b.seats.flatMap((s, i) => (i === viewer ? [] : s.hand)), ...b.deck];
            pool.push(pool.shift()!);
            let at = 0;
            const seats = b.seats.map((s, i) => {
                if (i === viewer) return s;
                const hand = pool.slice(at, at + s.hand.length);
                at += s.hand.length;
                return { ...s, hand };
            });
            const moved = rebuild({ ...b, seats, deck: pool.slice(at) }).build();
            assert.notEqual(hex(moved.state), hex(row.state), `game ${g} viewer ${viewer}: the hidden cards really moved`);
            const after = envelope({ gameId: row.gameId, ...moved }, viewer, 1);

            assert.equal(hex(after), hex(before), `game ${g} viewer ${viewer}: the view changed when a hidden hand changed`);
            compared++;

            // And the counts really were non-trivial - a game with empty hands
            // would satisfy the above for the wrong reason.
            const hidden = b.seats.reduce((n, s, i) => n + (i === viewer ? 0 : s.hand.length), 0);
            assert.ok(hidden > 0, `game ${g} viewer ${viewer}: nothing was actually being masked`);
        }
    }
    assert.ok(compared >= 20, `expected a spread of viewers, got ${compared}`);
});

test('view codec: every viewer reads the committed board, masked, for every seat and the spectator', () => {
    const GAMES = Number(process.env.VIEW_GAMES || 10);
    let checks = 0, ends = 0;

    for (let g = 0; g < GAMES; g++) {
        const np = 2 + (g % 5); // 2..6 players (36- and 52-card decks)
        let row = dealTable(seatsFor(np), seedBytes(np, 6151 * g + 17), { gameId: 'viewgame' });

        const checkAllSeats = (mv: number) => {
            const b = boardOf(row);
            for (let seat = -1; seat < np; seat++) {
                checkView(row, b, seat, `game ${g} move ${mv} viewer ${seat}`);
                checks++;
            }
        };
        checkAllSeats(0); // the fresh deal: full hands, flipped trump, no battles

        for (let mv = 1; mv <= 250 && row.status === L.GAME_STATUS_PLAYING; mv++) {
            const menu = legalMoves(boardOf(row));
            if (menu.length === 0) break;
            const m = menu[ri(menu.length)];
            const next = tableMove(row, m.playerId, m.wire);
            assert.ok(typeof next !== 'number', `game ${g} move ${mv}: the kernel's own legal move applies (${next})`);
            row = next;
            // The game end is the table's (finalize in table_act), so the codec is also
            // proven on GAME_OVER states: the elimination order, parked statuses, the table cleared.
            if (row.status === L.GAME_STATUS_GAME_OVER) ends++;
            if (mv % 3 === 0 || row.status !== L.GAME_STATUS_PLAYING) checkAllSeats(mv);
        }
    }

    assert.ok(checks > 300, `exercised enough view round-trips (${checks})`);
    assert.ok(ends >= 2, `enough games reached GAME_OVER under the codec check (${ends})`);
    console.error(`[view codec] ${checks} seat round-trips, ${ends} finished games`);
});
