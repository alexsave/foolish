// The kernel refuses a Game it could never have produced.
//
// Every game enters the kernel as bytes: the server loads games.state with its
// roster (table_load, c/src/table.h) for every operation, and a board a client
// or a probe holds is imported masked (wasm_import_state(1), the door
// client_validate and S1's re-encoder use). The decoder clamps COUNTS so it
// cannot corrupt memory, but it used to adopt any VALUE as it came - a defender
// seat past the table, an eliminated seat that is not seated, a status nobody
// knows, a card byte that is no card - and then play on it.
//
// Each case takes a real dealt table, writes ONE impossible value onto the
// kernel's own board through the generated setters (sdk/ts/gen/game_layout.bots.ts),
// has the kernel serialize that board, and hands the bytes back through the
// doors. It asserts three things: the load is refused with the kernel's
// GAME_INVALID_* reason (c/src/game.h game_validate, named by the generated
// constants), a refused table is not played on (an action answers
// TABLE_E_NOT_LOADED), and the kernel did not adopt the refused state - the
// board resident before the refusal is still the one resident after it. The
// value checks live in C; nothing here re-derives them. Pure kernel test - no
// Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import { fixture, fixtureExports, fixtureTable, reasonOf, IDLE, READY, type TableFixture } from './helpers/table_fixture.ts';
import { legalMoves, residentBoard } from './helpers/table_play.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

/** The raw doors of the fixtures' bots.wasm instance this suite knocks on (c/wasm/wasm_api.c). */
interface Doors {
    memory: WebAssembly.Memory;
    wasm_io_ptr(): number;
    wasm_game_ptr_internal(): number;
    /** The resident board as a durable blob into the IO buffer: [version][deterministic flag][state]. */
    wasm_state_serialize(): number;
    /** The resident board's state (no prefix) into the IO buffer. */
    wasm_export_state(): number;
    /** The IO buffer's state into the resident board, masked or whole: GAME_VALID or GAME_INVALID_*. */
    wasm_import_state(masked: number): number;
}
const doors = (): Doors => fixtureExports() as unknown as Doors;
const mem = () => L.memOf(doors().memory.buffer);
const io = (n: number) => mem().u8.slice(doors().wasm_io_ptr(), doors().wasm_io_ptr() + n);

const GID = 'validate';
const SEATS = Array.from({ length: 4 }, (_, i) => ({ id: `player-${i}`, name: `Player ${i}` }));

/** A dealt 4-seat table: trump up, hands of six, the first attacker to move. */
function dealt(seedByte = 7): TableFixture {
    const table = fixtureTable();
    let b = fixture().title(GID).seats(SEATS);
    for (let s = 1; s < SEATS.length; s++) b = b.seatStatus(s, READY);
    const lobby = b.build();
    assert.equal(table.load(lobby.state, lobby.roster), L.TABLE_OK, 'the lobby loads');
    assert.equal(table.ready(SEATS[0].id, new Uint8Array(32).fill(seedByte)), L.TABLE_OK, 'the last ready deals');
    const p = table.commit(GID, 1, 0);
    assert.ok(typeof p !== 'number', `commit products (${p})`);
    assert.equal(p.status, L.GAME_STATUS_PLAYING, 'fixture is dealt');
    return { state: p.state, roster: p.roster };
}

/** The resident board's durable blob. */
function residentBlob(): Uint8Array {
    return io(doors().wasm_state_serialize());
}

/** Loads `fx`, lets `poison` write one value onto the resident board, and returns the kernel's blob of it. */
function poisoned(fx: TableFixture, poison: (m: L.Mem, g: number) => void): Uint8Array {
    assert.equal(fixtureTable().load(fx.state, fx.roster), L.TABLE_OK, 'the real table loads');
    const g = doors().wasm_game_ptr_internal();
    poison(mem(), g);
    return residentBlob();
}

const refusedAs = (rc: number, want: number, what: string) =>
    assert.equal(rc, want, `${what}: refused as ${reasonOf(want, ['GAME_INVALID_'])}, got ${reasonOf(rc, ['GAME_INVALID_', 'TABLE_E_'])} (${rc})`);

/** The opening attack's wire, as the kernel enumerates it for the first attacker. */
function openingAttack(fx: TableFixture): { actorId: string; wire: Uint8Array } {
    assert.equal(fixtureTable().load(fx.state, fx.roster), L.TABLE_OK);
    const b = residentBoard(GID, fx.state, fx.roster);
    const m = legalMoves(b).find((x) => x.kind === 'attack' && x.seat === b.firstAttacker);
    assert.ok(m, 'the first attacker has an attack');
    return { actorId: m.playerId, wire: m.wire };
}

// One impossible value per GAME_INVALID_* family, written the way a corrupted
// row or a hostile client would carry it.
// `masked: false` marks a value in a slot a masked board does not carry as an
// identity (the stock is a count of hidden cards; hidden cards are not compared).
const FAMILIES: { name: string; reason: number; masked?: false; poison: (m: L.Mem, g: number) => void }[] = [
    { name: 'a defender seat past the table', reason: L.GAME_INVALID_SEAT,
      poison: (m, g) => L.Game_set_defender(m, g, SEATS.length) },
    { name: 'a first attacker seat past the table', reason: L.GAME_INVALID_SEAT,
      poison: (m, g) => L.Game_set_first_attacker(m, g, 9) },
    { name: 'an eliminated seat that is not seated', reason: L.GAME_INVALID_ELIMINATION,
      poison: (m, g) => { L.Game_set_elimination_order(m, g, 0, 6); L.Game_set_num_eliminated(m, g, 1); } },
    { name: 'a good bit for a seat that does not exist', reason: L.GAME_INVALID_GOOD_MASK,
      poison: (m, g) => L.Game_set_good_players_mask(m, g, 1 << 6) },
    { name: 'a game status outside the enum', reason: L.GAME_INVALID_STATUS,
      poison: (m, g) => L.Game_set_status(m, g, 9) },
    { name: 'a player status outside the enum', reason: L.GAME_INVALID_PLAYER_STATUS,
      poison: (m, g) => L.Player_set_status(m, L.Game_players_at(g, 2), 77) },
    { name: 'a card that is not a card', reason: L.GAME_INVALID_CARD,
      poison: (m, g) => L.Card_raw_set(m, L.Player_hand_at(L.Game_players_at(g, 1), 0), L.Card_pack(3, 15)) },
    { name: 'a deck card that is not a card', reason: L.GAME_INVALID_CARD, masked: false,
      poison: (m, g) => L.Card_raw_set(m, L.Game_deck_at(g, 0), L.Card_pack(2, 0)) },
    { name: 'one card in two hands', reason: L.GAME_INVALID_DUPLICATE_CARD, masked: false,
      poison: (m, g) => L.Card_raw_set(m, L.Player_hand_at(L.Game_players_at(g, 1), 0),
          L.Card_raw_get(m, L.Player_hand_at(L.Game_players_at(g, 0), 1))) },
    { name: 'a face-up trump of another suit than the power suit', reason: L.GAME_INVALID_FLIPPED,
      poison: (m, g) => L.Game_set_power_suit(m, g, (L.Game_get_power_suit(m, g) + 1) % 4) },
    { name: 'cards held under a WAITING status', reason: L.GAME_INVALID_LOBBY_CARDS,
      poison: (m, g) => L.Game_set_status(m, g, L.GAME_STATUS_WAITING) },
];

// ---- the server's door: table_load of games.state and games.roster -----------

for (const f of FAMILIES) {
    test(`table_load refuses ${f.name}, and the refused table is not played on`, () => {
        const fx = dealt();
        const { actorId, wire } = openingAttack(fx);
        const bad = poisoned(fx, f.poison);
        const table = fixtureTable();
        refusedAs(table.load(bad, fx.roster), f.reason, f.name);
        assert.equal(table.act(actorId, wire, null, 0), L.TABLE_E_NOT_LOADED, `${f.name}: an action on the refused table is not applied`);
        assert.equal(typeof table.envelope(GID, 0, 1), 'number', `${f.name}: nor is an envelope written from it`);
    });
}

test('a refused blob leaves the previously loaded board resident', () => {
    // Build both first: a deal runs the kernel, so it replaces the resident board.
    const good = dealt(7);
    const bad = poisoned(dealt(8), (m, g) => L.Game_set_first_attacker(m, g, 0x7f));
    const table = fixtureTable();
    assert.equal(table.load(good.state, good.roster), L.TABLE_OK, 'load `good`: now resident');
    const expected = residentBlob();
    assert.deepEqual([...expected], [...good.state], 'the resident blob is the loaded one');

    refusedAs(table.load(bad, good.roster), L.GAME_INVALID_SEAT, 'the bad blob');

    assert.deepEqual([...residentBlob()], [...expected], 'the refused blob was not adopted');
});

// ---- the masked door: a client's board (wasm_import_state(masked=1)) -----------

test('the masked importer refuses the same values, and leaves the resident board as it was', () => {
    const fx = dealt();
    let checked = 0;
    for (const f of FAMILIES) {
        if (f.masked === false) continue;
        assert.equal(fixtureTable().load(fx.state, fx.roster), L.TABLE_OK);
        const before = residentBlob();
        f.poison(mem(), doors().wasm_game_ptr_internal());
        const badState = io(doors().wasm_export_state());
        assert.equal(fixtureTable().load(fx.state, fx.roster), L.TABLE_OK, 'the real board is resident again');
        mem().u8.set(badState, doors().wasm_io_ptr());
        refusedAs(doors().wasm_import_state(1), f.reason, `masked: ${f.name}`);
        assert.deepEqual([...residentBlob()], [...before], `masked: ${f.name}: the refused board was not adopted`);
        checked++;
    }
    assert.equal(checked, FAMILIES.filter((f) => f.masked !== false).length, 'every family a masked board carries went through the masked door');
    assert.ok(checked >= 8, `enough families (${checked})`);
});

// ---- the refusal does not reach legitimate games ----------------------------

test('a dealt game, its played-on blob and a lobby all still load', () => {
    const fx = dealt();
    const table = fixtureTable();
    assert.equal(table.load(fx.state, fx.roster), L.TABLE_OK, 'the dealt blob loads');
    assert.deepEqual([...residentBlob()], [...fx.state], 'and round-trips byte for byte');
    assert.ok(table.envelope(GID, 1, 1) instanceof Uint8Array, 'an envelope is written from it');

    const { actorId, wire } = openingAttack(fx);
    assert.equal(table.act(actorId, wire, null, 0), L.TABLE_APPLIED, 'the opening attack applies');
    const p = table.commit(GID, 2, 0);
    assert.ok(typeof p !== 'number', `commit products (${p})`);
    assert.equal(table.load(p.state, p.roster), L.TABLE_OK, 'the played-on blob loads');

    const lobby = fixture().title(GID).seats(SEATS).seatStatus(1, READY).build();
    assert.equal(table.load(lobby.state, lobby.roster), L.TABLE_OK, 'a lobby loads');
    assert.deepEqual(residentBoard(GID, lobby.state, lobby.roster).seats.map((s) => s.status), [IDLE, READY, IDLE, IDLE]);
    // And an attack wire is still refused by the rules, not the validator, on a lobby.
    assert.equal(table.act(SEATS[0].id, encodeAction({ kind: 'attack', cards: [{ suit: 0, value: 5 }] }), null, 0), L.TABLE_REJECTED,
        'a move on a lobby is a rules refusal');
});
