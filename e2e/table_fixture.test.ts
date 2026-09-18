/* =============================================================================
 * C-backed test fixtures: a board a test writes is a row the kernel accepted
 * =============================================================================
 * docs/C_GAME_SHAPE_MIGRATION.md Phase 3c. e2e/helpers/table_fixture.ts builds a
 * game through the generated setters and the kernel's card notation, and seals
 * it with table_seal; e2e/helpers/table_db.ts writes it as a kernel-owned games
 * row. What is pinned here:
 *
 *   - a fixture the kernel would refuse throws FixtureRefused carrying the
 *     kernel's reason code and its generated name, one case per refusal family:
 *     a card in two places, a lobby holding a hand, a state whose seat count is
 *     not the roster's, a brain this build does not link, card text that is not
 *     a card, a roster the Roster refuses;
 *   - an accepted fixture round-trips: table_load takes its bytes, the commit
 *     writes them back unchanged, and table_envelope shows every field the
 *     builder set (read by the web's own client slot, as a TableView);
 *   - seedTable stores exactly those bytes, the kernel's status and needs_bots,
 *     and the membership rows, and the stored row loads again.
 * ========================================================================== */

import { applySchema, pgPool, uuid } from './harness.ts';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { readEnvelopeView } from './helpers/client_read.ts';
import { NO_CARD } from '../src/state/view.ts';
import { fixture, fixtureTable, FixtureRefused, GAME_OVER, IDLE, IN, OUT, PLAYING, READY } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';

const refusedWith = (code: number, name: string, detail?: string) => (e: unknown) => {
    assert.ok(e instanceof FixtureRefused, `a FixtureRefused, got ${String(e)}`);
    assert.equal(e.code, code, `code: ${e.message}`);
    assert.equal(e.reason, name);
    if (detail) assert.equal(e.detail?.reason, detail);
    return true;
};

const two = () => fixture().seats([{ id: 'ann', name: 'Ann' }, { id: 'bob', name: 'Bob' }]);

test('refused: one card in two places', () => {
    assert.throws(() => two().status(PLAYING).hand(0, '6h 7h').hand(1, '7h').build(),
        refusedWith(L.GAME_INVALID_DUPLICATE_CARD, 'GAME_INVALID_DUPLICATE_CARD'));
    // 10 and T are one rank: the notation cannot hide a duplicate.
    assert.throws(() => two().status(PLAYING).hand(0, '10d').table('Td').build(),
        refusedWith(L.GAME_INVALID_DUPLICATE_CARD, 'GAME_INVALID_DUPLICATE_CARD'));
});

test('refused: a WAITING table holding a hand', () => {
    assert.throws(() => two().hand(0, '6h').build(),
        refusedWith(L.GAME_INVALID_LOBBY_CARDS, 'GAME_INVALID_LOBBY_CARDS'));
});

test('refused: a seat count that disagrees with the roster', () => {
    assert.throws(() => two().status(PLAYING).numPlayers(3).build(),
        refusedWith(L.TABLE_E_MISMATCH, 'TABLE_E_MISMATCH'));
});

test('refused: a bot brain this build does not link', () => {
    assert.throws(() => fixture().seats([{ id: 'ann', name: 'Ann' }, { id: 'b', name: 'B', brain: 'nosuchbrain' }]).build(),
        refusedWith(L.TABLE_E_UNKNOWN_BRAIN, 'TABLE_E_UNKNOWN_BRAIN'));
});

test('refused: card text that is not a card, with the notation reason', () => {
    assert.throws(() => two().status(PLAYING).hand(0, '6h 7x').build(), refusedWith(L.CARD_PARSE_E_SUIT, 'CARD_PARSE_E_SUIT'));
    assert.throws(() => two().status(PLAYING).deck('1h').build(), refusedWith(L.CARD_PARSE_E_RANK, 'CARD_PARSE_E_RANK'));
    assert.throws(() => two().status(PLAYING).table('7c/').build(), refusedWith(L.CARD_PARSE_E_EMPTY, 'CARD_PARSE_E_EMPTY'));
    assert.throws(() => two().status(PLAYING).hand(1, '7c/8c').build(), refusedWith(L.CARD_PARSE_E_SYNTAX, 'CARD_PARSE_E_SYNTAX'));
    assert.throws(() => two().status(PLAYING).trump('As Ks').build(), refusedWith(L.CARD_PARSE_E_CAP, 'CARD_PARSE_E_CAP'));
    assert.throws(() => two().status(PLAYING).trump('').build(), refusedWith(L.CARD_PARSE_E_EMPTY, 'CARD_PARSE_E_EMPTY'));
});

test('refused: a roster the Roster refuses, at the seat that broke it', () => {
    assert.throws(() => fixture().seats([{ id: 'ann', name: 'Ann' }, { id: 'ann', name: 'Ann again' }]).build(),
        (e: unknown) => refusedWith(L.ROSTER_E_DUPLICATE, 'ROSTER_E_DUPLICATE')(e) && (e as FixtureRefused).step === 'seat 1');
});

const GID = 'fixture-game';
const card = (suit: number, value: number) => ({ suit, value });

test('accepted: a dealt game round-trips through table_load and table_envelope', () => {
    const fx = fixture()
        .title('Round trip')
        .seats([{ id: 'ann', name: 'Ann' }, { id: 'bot', name: 'Robo', brain: 'cordite' }, { id: 'cid', name: 'Cid' }])
        .status(PLAYING)
        .hand(0, '6h 7h Qs').hand(1, '8d, 9d').hand(2, 'Ac')
        .table('7c/8c', '9s')
        .deck('Ts Jd')
        .trump('As')
        .attacker(1).defender(2)
        .discard(4)
        .good(0).goodTimestamp()
        .awaiting(0)
        .build();

    const table = fixtureTable();
    assert.equal(table.load(fx.state, fx.roster), L.TABLE_OK, 'table_load accepts the fixture');
    assert.deepEqual(table.seats(), [
        { id: 'ann', name: 'Ann', brain: '' }, { id: 'bot', name: 'Robo', brain: 'cordite' }, { id: 'cid', name: 'Cid', brain: '' },
    ]);
    assert.equal(table.needsBots(), true, 'PLAYING with a bot seat IN');

    const p = table.commit(GID, 7, 0);
    assert.ok(typeof p !== 'number', `commit products (${p})`);
    assert.deepEqual(p.state, fx.state, 'the loaded table writes the fixture blob back unchanged');
    assert.deepEqual(p.roster, fx.roster, 'and the roster');
    assert.equal(p.status, L.GAME_STATUS_PLAYING);

    const env = table.envelope(GID, 0, 7);
    assert.ok(env instanceof Uint8Array, `table_envelope (${env})`);
    const g = readEnvelopeView(env);
    assert.ok(g, 'the envelope reads');
    assert.equal(g.mySeat, 0);
    assert.equal(g.title, 'Round trip');
    assert.equal(g.status, L.GAME_STATUS_PLAYING);
    assert.deepEqual(g.myHand, [card(L.SUIT_HEARTS, 5), card(L.SUIT_HEARTS, 6), card(L.SUIT_SPADES, 11)], "seat 0's hand, in order");
    assert.equal(g.seats[0].awaitingAttack, true);
    assert.deepEqual(g.seats.map((s) => [s.id, s.name, s.isAi, s.status, s.handCount]), [
        ['ann', 'Ann', false, IN, 3], ['bot', 'Robo', true, IN, 2], ['cid', 'Cid', false, IN, 1],
    ]);
    assert.deepEqual(g.battles, [
        { attack: card(L.SUIT_CLUBS, 6), defense: card(L.SUIT_CLUBS, 7) },
        { attack: card(L.SUIT_SPADES, 8), defense: NO_CARD },
    ]);
    assert.equal(g.deckCount, 2);
    assert.equal(g.hasFlipped, true);
    assert.deepEqual(g.flipped, card(L.SUIT_SPADES, 13));
    assert.equal(g.powerSuit, L.SUIT_SPADES, 'the power suit is the trump suit');
    assert.equal(g.firstAttacker, 1);
    assert.equal(g.defender, 2);
    assert.equal(g.discardPileLength, 4);
    assert.equal(g.goodMask, 1 << 0, 'ann said good');
    assert.equal(g.hasGoodTimestamp, true);
});

test('accepted: a lobby, a finished game, eliminations and seat defaults', () => {
    const table = fixtureTable();
    const seats = [{ id: 'h1', name: 'Hana' }, { id: 'b1', name: 'Bolt', brain: 'random' }, { id: 'h2', name: 'Ivo' }];

    const lobby = fixture().seats(seats).seatStatus(2, READY).build();
    assert.equal(table.load(lobby.state, lobby.roster), L.TABLE_OK);
    assert.equal(table.needsBots(), false, 'a lobby needs no bots');
    const lg = readEnvelopeView(table.envelope(GID, -1, 0) as Uint8Array)!;
    assert.equal(lg.status, L.GAME_STATUS_WAITING);
    assert.deepEqual(lg.seats.map((s) => s.status), [IDLE, READY, READY], 'human IDLE and bot READY by default; seatStatus overrides');

    const playing = fixture().seats(seats).status(PLAYING).hand(0, '6c').hand(2, '7c').eliminated(1).powerSuit(L.SUIT_DIAMONDS).build();
    assert.equal(table.load(playing.state, playing.roster), L.TABLE_OK);
    assert.equal(table.needsBots(), false, 'the only bot is out');
    const pg = readEnvelopeView(table.envelope(GID, -1, 0) as Uint8Array)!;
    assert.deepEqual(pg.seats.map((s) => s.status), [IN, OUT, IN], 'an eliminated seat is OUT while PLAYING');
    assert.deepEqual(pg.elimination, [1]);
    assert.equal(pg.hasFlipped, false);
    assert.equal(pg.powerSuit, L.SUIT_DIAMONDS);
    assert.equal(pg.defender, 1, 'the defender defaults to the seat after the attacker');

    const over = fixture().seats(seats).status(GAME_OVER).hand(2, 'Kh').eliminated(1, 0).build();
    assert.equal(table.load(over.state, over.roster), L.TABLE_OK);
    const og = readEnvelopeView(table.envelope(GID, -1, 0) as Uint8Array)!;
    assert.equal(og.status, L.GAME_STATUS_GAME_OVER);
    assert.deepEqual(og.seats.map((s) => s.status), [IDLE, READY, IDLE], 'a finished game parks bots READY and humans IDLE');
    assert.deepEqual(og.elimination, [1, 0]);

    const seeded = fixture().seats(seats.slice(0, 2)).status(PLAYING).deck('9h Th').deterministic().build();
    assert.equal(seeded.state[1], 1, 'the durable blob carries the deterministic-deck flag');
    assert.equal(table.load(seeded.state, seeded.roster), L.TABLE_OK);
});

test('seedTable: the stored row is the fixture, with the kernel status, needs_bots and members', async () => {
    await applySchema();
    const human = uuid(), bot = uuid(), other = uuid(), gameId = `fx${uuid().slice(0, 6)}`;
    const fx = fixture()
        .seats([{ id: human, name: 'Hana' }, { id: bot, name: 'Bolt', brain: 'cordite' }, { id: other, name: 'Ivo' }])
        .status(PLAYING).hand(0, '6h').hand(1, '7h 8h').hand(2, 'Qs').trump('9d').deck('Ad')
        .build();
    await seedTable(gameId, fx, { version: 5 });

    const row = (await pgPool.query('SELECT status, state, roster, needs_bots, writer_gen, version FROM games WHERE id = $1', [gameId])).rows[0];
    const hex = (b: Uint8Array) => `\\x${Buffer.from(b).toString('hex')}`;
    assert.equal(row.status, 'playing');
    assert.equal(row.state, hex(fx.state), 'games.state is the fixture blob');
    assert.equal(row.roster, hex(fx.roster), 'games.roster is the fixture roster');
    assert.equal(row.needs_bots, true, 'the kernel says a bot is IN');
    assert.equal(row.writer_gen, 2, 'owned by the kernel writers, so the legacy bridge derived nothing');
    assert.equal(Number(row.version), 5);

    const humans = (await pgPool.query('SELECT player_id FROM player_hands WHERE game_id = $1 ORDER BY player_id', [gameId])).rows.map((r) => r.player_id);
    assert.deepEqual(humans, [human, other].sort());
    const bots = (await pgPool.query('SELECT h.bot_id, b.strategy_key, b.nickname FROM bot_hands h JOIN bots b ON b.id = h.bot_id WHERE h.game_id = $1', [gameId])).rows;
    assert.deepEqual(bots, [{ bot_id: bot, strategy_key: 'cordite', nickname: 'Bolt' }]);

    const bytes = (h: string) => Uint8Array.from(Buffer.from(h.slice(2), 'hex'));
    assert.equal(fixtureTable().load(bytes(row.state), bytes(row.roster)), L.TABLE_OK, 'the stored row loads');

    const lobbyId = `fx${uuid().slice(0, 6)}`;
    await seedTable(lobbyId, fixture().seats([{ id: human, name: 'Hana' }, { id: bot, name: 'Bolt', brain: 'cordite' }]).build());
    const lobby = (await pgPool.query('SELECT status, needs_bots FROM games WHERE id = $1', [lobbyId])).rows[0];
    assert.deepEqual(lobby, { status: 'waiting', needs_bots: false });
});
